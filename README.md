# esp32-C3-study

ESP32-C3 SuperMini 学习记录：从 hello world 做到 DHT11 温湿度 + OLED 实时显示。

每个子目录都是一个独立的 ESP-IDF 工程，按编号递进，可以单独编译烧录。

| 工程 | 内容 | 新增 |
|------|------|------|
| 00_helloworld | 串口打印 | —— |
| 01_blink | 板载 LED 闪烁 | `LED` 组件 |
| 02_oled | OLED 显示数字和 LED 状态 | `OLED` 组件（SSD1306 SPI） |
| 03_DHT11_oled | 实时显示温湿度 | `DHT11` 组件 + FreeRTOS 双任务 + 队列 |

> 03 的详细说明（接线、协议拆解、调试）在 [03_DHT11_oled/README.md](03_DHT11_oled/README.md)

## 硬件与接线

ESP32-C3 SuperMini + 0.96" SSD1306 OLED（SPI）+ DHT11。

| GPIO | 用途 |
|------|------|
| 1 / 3 / 4 / 6 / 7 | OLED RES / DC / D0 / D1 / CS |
| 5 | DHT11 DATA（必须有上拉电阻） |
| 8 | 板载 LED（低电平点亮） |

## 编译烧录

VS Code 里点左下角的 ▶ / 🔥 按钮即可，端口已配在 `.vscode/settings.json` 里。

命令行：

```bash
cd 03_DHT11_oled
idf.py -p /dev/tty.usbmodem101 build
idf.py -p /dev/tty.usbmodem101 flash
idf.py -p /dev/tty.usbmodem101 monitor
```

端口 macOS 上是 `/dev/tty.usbmodem101`，Windows 上是 `COM3`。

## 目录结构

```
esp32-C3-study/
├── 00_helloworld/              模板工程
├── 01_blink/
│   ├── main/main.c
│   └── components/LED/         板载 LED 驱动
├── 02_oled/
│   ├── main/main.c
│   └── components/{LED,OLED}/
├── 03_DHT11_oled/
│   ├── main/main.c             双任务 + 队列
│   ├── components/{DHT11,LED,OLED}/
│   ├── tools/                  主机端单元测试，make 即可跑，不用接硬件
│   └── README.md
└── .gitignore                  build/ 等编译产物不入库
```

## 几点提醒

- `vTaskDelay()` 的单位是 tick（10ms），要毫秒得用 `pdMS_TO_TICKS()`。
- 四个工程的 `project()` 都还叫 `00_helloworld`，编译产物也叫 `00_helloworld.bin`。
- DHT11 起始信号要求 ≥18ms，`vTaskDelay()` 精度不够，代码里用 `esp_timer` 精确定时。
- 换机器记得改 `.vscode/settings.json` 里的 `idf.currentSetup` 和 `idf.port`。
