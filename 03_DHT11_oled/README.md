# 03_DHT11_oled —— DHT11 温湿度 + SSD1306 OLED 显示

ESP32-C3 SuperMini + DHT11 单总线传感器 + 0.96" SSD1306 OLED(SPI)。
DHT11 每 2 秒采样一次，温湿度通过队列交给显示任务刷新到屏幕上。

```
   dht_task (prio 5)                 display_task (prio 4)
   ------------------                ------------------
   每 2 秒读一次 DHT11               阻塞等队列
        |                                   ^
        v                                   |
   xQueueSend(dht11_data_t)  -------->  xQueueReceive()
```

---

## 1. 接线

### DHT11（三脚 / 四脚模块都适用）

| DHT11 | ESP32-C3 | 说明 |
|-------|----------|------|
| VCC   | 3V3      | 千万别接 5V |
| DATA  | GPIO5    | 单总线，需要上拉 |
| GND   | GND      | |

> **必须有上拉电阻**。常见的三针模块板上自带一颗 10K；如果是裸的 DHT11 传感器，
> 请在 DATA 和 3V3 之间接一个 4.7K~10K 电阻，否则读出来全是 0 或校验失败。

### OLED（SSD1306，四针 SPI 版）

沿用 02_oled 工程已经焊好的接线：

| OLED | ESP32-C3 |
|------|----------|
| VCC  | 3V3      |
| GND  | GND      |
| DO   | GPIO4    |
| D1   | GPIO6    |
| RES  | GPIO1    |
| DC   | GPIO3    |
| CS   | GPIO7    |

> **屏黑先怀疑 SPI 数据/时钟接反**。中景园这块模块的丝印是 D0/D1，
> 但厂商例程注释写的是「D0 = SCL(时钟)」「D1 = SDA(数据)」——和常规接法相反。
> 代码里由 `components/OLED/include/OLED.h` 的 `OLED_SPI_SWAPPED` 宏统一控制，
> 换模块时只改这一个宏。

板载 LED 在 GPIO8（低电平点亮），读数成功时点亮，读数失败时熄灭。

---

## 2. 目录结构

```
03_DHT11_oled/
├── main/
│   └── main.c              入口：建队列、起两个任务
├── components/
│   ├── DHT11/              DHT11 驱动（本次新增）
│   │   ├── dht11.c
│   │   └── include/dht11.h
│   ├── OLED/               从 02_oled 原样复制（SSD1306 SPI 驱动）
│   └── LED/                板载 LED（GPIO8）
└── tools/                  主机端单元测试（不接硬件也能验证解码逻辑）
    ├── test_decode.c
    ├── Makefile
    └── stubs/              最小 ESP-IDF 桩头文件
```

---

## 3. 编译 / 烧录 / 监视

在 ESP-IDF 终端（`C:\esp\v6.1\esp-idf`）里：

```bash
cd C:/Users/lqr_l/Desktop/esp32/03_DHT11_oled

idf.py -p COM3 build      # 编译
idf.py -p COM3 flash     # 烧录
idf.py -p COM3 monitor   # 看串口日志（Ctrl+] 退出）
```

VS Code 里直接用左下角的 ▶ / 🔥 按钮也可以，端口在 `.vscode/settings.json` 里
已经配成 `COM3`，芯片目标 `esp32c3`，调试配置 `board/esp32c3-builtin.cfg`。

> 工程名目前还是模板里的 `00_helloworld`（`CMakeLists.txt` 里的 `project()`），
> 所以编译产物叫 `00_helloworld.bin`。想改名的话改 `project(03_DHT11_oled)`，
> 然后**删掉 build 目录重新 configure**，否则 CMake 会因为工程名变了而报错。

---

## 4. 预期现象

串口（115200）：

```
I (xxx) dht11: DHT11 init on GPIO5
I (xxx) app: dht task started, pin=GPIO5, period=2000ms
I (xxx) app: temp=25.0 C  humi=60.0 %
I (xxx) app: temp=25.0 C  humi=61.0 %
...
```

屏幕（8x16 字库，每行 12 个字符）：

```
┌────────────────┐
│DHT11           │   ← 标题
│Temp: 25.0C     │
│Humi: 60.0%     │
│Status:  OK     │
└────────────────┘
```

读数失败时温度/湿度两行显示 `---.-`，状态行显示 `Status: ERR`，同时 LED 熄灭。

---

## 5. DHT11 协议要点（代码里对应的地方）

| 阶段 | 时序 | 代码位置 |
|------|------|----------|
| 起始信号 | 主机拉低 ≥ 18ms 再释放 | `dht11_read_raw()` Phase A |
| 应答 | 从机低 80us + 高 80us | `dht11_decode()` 里按脉宽搜索 |
| 数据位 | 低 50us，高 26us=`0` / 70us=`1` | `DHT11_BIT_HIGH_US` 阈值判定 |
| 帧长 | 40 位 = 湿度整数/湿度小数/温度整数/温度小数/校验和 | `dht11_read()` 解析 |
| 校验 | `b4 == (b0+b1+b2+b3) & 0xFF` | `dht11_decode()` 末尾 |

驱动用 **GPIO 双边沿中断 + `esp_timer_get_time()` 抓边沿时间戳**，而不是任务轮询电平：
26us 和 70us 的差别太窄，轮询一旦被 FreeRTOS 抢占就会读错；中断抓时间戳则完全不受调度影响。
解码放在任务里做，ISR 只负责记时间戳。

采样周期取 2 秒（DHT11 手册要求至少 1 秒）。

---

## 6. 曾经踩的坑：一直显示 Status: ERR

这个工程第一次跑起来时，OLED 一直是 `Status: ERR`，读数永远是 `---.-`。
根因不在解码逻辑，而在**起始信号的时间长度**：

```c
gpio_set_direction(s_pin, GPIO_MODE_OUTPUT);
vTaskDelay(pdMS_TO_TICKS(20));      /* ← 以为延时了 20ms */
```

ESP-IDF 默认 FreeRTOS tick 是 **100Hz（10ms/tick）**，`pdMS_TO_TICKS(20)` 只有 2 个 tick，
而 `vTaskDelay(2 ticks)` 的实际延时是 **(10ms, 20ms]**——取决于调用那一刻落在 tick 周期的哪个位置。
DHT11 要求起始低电平 **≥18ms** 才会应答，短了它就当没收到请求，**一个边沿都不回**。

更坑的是：dht_task 是周期性任务，2 秒一个周期正好是 200 个 tick，和 tick 计数器**相位恒定**，
所以这个延时每次都是同一个偏短的值 → 不是偶尔失败，而是**每次都失败**。

修法是用 `esp_timer` 精确定时（`components/DHT11/dht11.c` 的 `dht11_delay_ms()`）：
剩余时间多时睡一个 tick，最后 12ms 用 `taskYIELD()` 忙等补足，保证实际延时 ≥ 目标值。

> 同理，工程里任何**微秒级/毫秒级精度的协议时序**（DS18B20、74HC595、RC522 等）
> 都不能直接用 `vTaskDelay()`，必须换算成 tick 之外的时间基准。

## 7. 常见问题

**读不到数据 / 校验和失败**
1. 先量一下 DATA 空闲时是不是高电平——没有上拉电阻最常见；
2. 接线：DATA 是不是接到了 GPIO5（不是 GPIO4，OLED 占着 GPIO4）；
3. DHT11 供电要 3V3 且能供出 3~5mA 的峰值电流，劣质模块很容易带不动；
4. 传感器离 ESP32 太近时会有干扰，拉一根 10cm 排线通常就好了；
5. 看串口日志里 `dht11:` 那一行：`no response` = 传感器没应答（查接线/上拉/起始信号），
   `decode failed` = 应答了但解不开（会额外打印抓到的边沿波形，看 dt 值是否在 26us/70us 附近）。

**模块其实是 DHT22**
有些厂家把 DHT22（AM2302）当 DHT11 卖。两者帧格式兼容，驱动会自动识别
（湿度整数 > 100 判定为 DHT22），串口日志会打印 `(DHT22)`。

**屏幕全黑**
`components/OLED/include/OLED.h` 里把 `OLED_SPI_SWAPPED` 从 1 改成 0 试试（见上面接线说明）。

**显示有残影**
`OLED_ShowString()` 在同一个 (x, y) 位置重复调用时会先清掉旧内容；
但如果两行文字写在**不同**的 y 上、长度又不同，残影不会被清，需要自己先
`OLED_ClearArea()`。

---

## 7. 主机端单元测试（不用接硬件）

`tools/` 下有一个直接调用 `dht11_decode()` 的测试，用合成的时间戳序列验证解码逻辑：

```bash
cd tools
make          # 需要 gcc，ESP-IDF 终端 / Git-Bash / WSL 都可以
```

覆盖的用例：正常 40 位帧、末位缺下降沿（39 位）、全 0、全 1、校验和错误、
帧被截断、起始位置偏移、±10us 时序抖动。当前 8 个用例全部通过。
