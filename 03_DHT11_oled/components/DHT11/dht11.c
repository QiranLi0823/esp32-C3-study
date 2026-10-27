/**
 * @file    dht11.c
 * @brief   DHT11 / DHT22 单总线温湿度传感器驱动 —— ESP32-C3
 *
 * 帧结构 (数据手册):
 *   主机: 拉低 >= 18ms  ->  释放总线(上拉)
 *   从机: 拉低 ~80us (应答低)  ->  拉高 ~80us (应答高)  ->  40 位数据
 *   每位: 低 ~50us, 然后高电平 26~28us 表示 '0', 70us 表示 '1'
 *   40 位 = 5 字节: 湿度整数 / 湿度小数 / 温度整数 / 温度小数 / 校验和
 *   校验和 = (b0 + b1 + b2 + b3) & 0xFF
 *
 * 实现要点:
 *   1. 用 GPIO 双边沿中断 + esp_timer_get_time() 记录每个边沿的时间戳,
 *      而不是靠任务轮询电平。DHT11 的 26us / 70us 高电平差别很窄,
 *      轮询一旦被 FreeRTOS 抢占就会读错, 中断抓时间戳则完全不受调度影响。
 *   2. 边沿在 ISR 里采集, 解码放在任务里做, 任务只影响「什么时候开始/结束采集」,
 *      不影响「脉宽测量精度」。
 *   3. 最后一位(校验和最低位)后面可能没有下降沿(传感器释放总线),
 *      这种情况下用校验和反推补全, 见 dht11_decode()。
 *   4. **起始信号必须 >= 18ms**, 而 vTaskDelay() 的延时是按 tick 量化的
 *      (默认 tick = 100Hz, pdMS_TO_TICKS(20) 实际只有 10~20ms),
 *      短了传感器根本不响应 —— 所以这里用 esp_timer 精确计时, 见 dht11_delay_ms()。
 *
 * 接线 (ESP32-C3 SuperMini):
 *   VCC  -> 3V3
 *   DATA -> GPIO5   (开漏单总线, 必须有上拉电阻; 常见 DHT11 模块板上自带 10K)
 *   GND  -> GND
 */

#include "dht11.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dht11";

/* 边沿缓冲: 一帧最多 1(释放) + 1(应答低) + 1(应答高) + 40*2(数据位) + 余量 */
#define DHT11_MAX_EDGES        160
#define DHT11_FRAME_MIN_EDGES  6
#define DHT11_FRAME_TIMEOUT_MS 40
/* 连续这么长时间没有新边沿就认为一帧结束 */
#define DHT11_SILENCE_US       3000

/* 起始信号: 主机拉低的时长。手册要求 >= 18ms, 这里给 20ms */
#define DHT11_START_LOW_MS     20
#define DHT11_START_LOW_MIN_MS 18

/* 应答段(低 80us / 高 80us)的判定窗口。
 *
 * 这里刻意放宽(30~250us), 因为真正把关的是最后的校验和:
 * 校验和 8 位, 误判概率只有 1/256, 比死抠 80us 更容易碰上个别批次的时序偏差。
 * 之所以还要区分应答低和数据位低, 是因为应答段总是出现在数据位之前,
 * 搜索时取第一个匹配即可。 */
#define DHT11_RESP_LOW_MIN_US  30
#define DHT11_RESP_LOW_MAX_US  250
#define DHT11_RESP_HIGH_MIN_US 20
#define DHT11_RESP_HIGH_MAX_US 250
/* 位判定阈值: '0' 高 26~28us, '1' 高 70us, 取中间 45us 两边都有余量 */
#define DHT11_BIT_HIGH_US      45
#define DHT11_BIT_HIGH_MAX_US  150

typedef struct {
    int64_t ts;      /* esp_timer_get_time(), 微秒 */
    uint8_t level;   /* 0 = 下降沿后为低, 1 = 上升沿后为高 */
} dht11_edge_t;

static dht11_edge_t s_edges[DHT11_MAX_EDGES];
static volatile uint32_t s_edge_count;
static volatile bool     s_capturing;
static gpio_num_t       s_pin = GPIO_NUM_5;
static bool             s_isr_ready;

/* 快照缓冲: 解码时用, 避免 ISR 正在写边沿数组 */
static dht11_edge_t s_snapshot[DHT11_MAX_EDGES];

/**
 * @brief 双边沿中断: 只做「记时间戳」这一件事
 */
static void dht11_edge_isr(void *arg)
{
    if (!s_capturing) {
        return;
    }

    uint32_t idx = s_edge_count;
    if (idx >= DHT11_MAX_EDGES) {
        return;         /* 缓冲满, 丢弃多余的边沿 */
    }

    s_edges[idx].ts    = esp_timer_get_time();
    s_edges[idx].level = gpio_get_level(s_pin);
    s_edge_count       = idx + 1;
}

/**
 * @brief 精确延时 ms 毫秒
 *
 * 为什么不用 vTaskDelay():
 *   ESP-IDF 默认 FreeRTOS tick 是 100Hz, pdMS_TO_TICKS(20) 只有 2 个 tick,
 *   而 vTaskDelay() 实际延时是 (n-1)*10ms < 实际 < n*10ms —— 也就是
 *   「延时 20ms」可能只延时了 11ms。DHT11 要求起始低电平 >= 18ms,
 *   短了它完全不应答, 于是每次读都是超时失败。
 *
 *   这里用 esp_timer 计时: 剩余时间多的时候睡一个 tick, 最后 12ms 用
 *   taskYIELD() 忙等补足, 保证实际延时 >= 目标值。
 */
static void dht11_delay_ms(int64_t ms)
{
    int64_t deadline = esp_timer_get_time() + ms * 1000;

    while (1) {
        int64_t remain = deadline - esp_timer_get_time();
        if (remain <= 0) {
            break;
        }
        if (remain > 12000) {
            vTaskDelay(pdMS_TO_TICKS(remain / 1000));   /* 粗睡, 留 12ms 忙等余量 */
        } else {
            taskYIELD();                                /* 细等, 保证不超时 */
        }
    }
}

/**
 * @brief 忙等 us 微秒(1us 分辨率)
 */
static void dht11_sleep_us(int64_t us)
{
    int64_t deadline = esp_timer_get_time() + us;

    while (esp_timer_get_time() < deadline) {
        taskYIELD();     /* 让出 CPU, 但不睡, 保持 1us 级分辨率 */
    }
}

esp_err_t dht11_init(gpio_num_t gpio_num)
{
    s_pin = gpio_num;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,      /* 单总线需要上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 显式再设一次内部上拉。
     * 后面每次读数据都会把引脚切成输出(拉低起始信号)再切回输入,
     * 不同 IDF 版本里 gpio_set_direction() 对上拉位的处理略有差别,
     * 这里显式拉一次上拉, 保证总线释放后仍然是高电平(不悬空)。
     */
    err = gpio_set_pull_mode(gpio_num, GPIO_PULLUP_ENABLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_pull_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_isr_ready) {
        /* 整个进程只装一次 ISR 服务 */
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
            return err;
        }
        s_isr_ready = true;
    }

    err = gpio_isr_handler_add(gpio_num, dht11_edge_isr, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "DHT11 init on GPIO%d", gpio_num);
    return ESP_OK;
}

/**
 * @brief 等待一帧采集完成
 *
 * 判据: 边沿数不再增加且持续超过 DHT11_SILENCE_US, 或者整体超时。
 */
static esp_err_t dht11_wait_frame(void)
{
    int64_t start = esp_timer_get_time();
    uint32_t last_count = 0;
    int64_t last_change = start;

    while (1) {
        uint32_t count = s_edge_count;
        int64_t now = esp_timer_get_time();

        if (count != last_count) {
            last_count  = count;
            last_change = now;
        }

        if (count >= DHT11_FRAME_MIN_EDGES && (now - last_change) >= DHT11_SILENCE_US) {
            break;
        }
        if ((now - start) > (int64_t)DHT11_FRAME_TIMEOUT_MS * 1000) {
            break;
        }

        dht11_sleep_us(100);
    }

    return (s_edge_count >= DHT11_FRAME_MIN_EDGES) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief 打印抓到的波形(调试用): 最多显示前 16 个边沿
 *
 * 格式: e[i] dt=xxus lvl=x, dt 是与上一个边沿的间隔
 */
static void dht11_dump_edges(const dht11_edge_t *e, uint32_t n)
{
    char buf[128];
    int  len = 0;

    len += snprintf(buf + len, sizeof(buf) - len, "edges=%u: ", (unsigned)n);

    for (uint32_t i = 0; i < n && i < 16; i++) {
        int64_t dt = (i == 0) ? 0 : (e[i].ts - e[i - 1].ts);
        len += snprintf(buf + len, sizeof(buf) - len,
                        "e%u(dt=%lld,lvl=%u) ", (unsigned)i,
                        (long long)dt, (unsigned)e[i].level);
        if (len >= (int)sizeof(buf) - 16) {
            break;
        }
    }

    ESP_LOGW(TAG, "%s", buf);
}

/**
 * @brief 从边沿序列里解出 5 个字节
 *
 * @return true 表示解出了合法(校验通过)的数据
 */
static bool dht11_decode(const dht11_edge_t *e, uint32_t n, uint8_t data[5])
{
    /*
     * 找应答段: 下降沿 -> 低电平 80us -> 上升沿 -> 高电平 80us -> 下降沿。
     * 数据位的低电平只有 50us, 用这个差别把应答段和数据段分开。
     * 起始位置可能有偏差(比如释放总线时的上升沿没抓到), 所以逐个尝试。
     */
    for (uint32_t f = 0; f + 4 < n; f++) {
        if (e[f].level != 0 || e[f + 1].level != 1 || e[f + 2].level != 0) {
            continue;
        }

        int64_t low  = e[f + 1].ts - e[f].ts;
        int64_t high = e[f + 2].ts - e[f + 1].ts;
        if (low  < DHT11_RESP_LOW_MIN_US  || low  > DHT11_RESP_LOW_MAX_US) {
            continue;
        }
        if (high < DHT11_RESP_HIGH_MIN_US || high > DHT11_RESP_HIGH_MAX_US) {
            continue;
        }

        /* 从这里开始是 40 位数据 */
        uint8_t bits[40] = {0};
        int     nbits    = 0;
        bool    bad      = false;

        for (int i = 0; i < 40; i++) {
            uint32_t r = f + 3 + 2 * i;          /* 该位的高电平起点(上升沿) */
            if (r >= n) {
                break;                          /* 帧被截断 */
            }
            if (e[r].level != 1) {
                bad = true;                     /* 期望上升沿, 实际不是 */
                break;
            }

            uint8_t bit;
            if (r + 1 < n && e[r + 1].level == 0) {
                int64_t dur = e[r + 1].ts - e[r].ts;
                if (dur > DHT11_BIT_HIGH_MAX_US) {
                    bad = true;
                    break;
                }
                bit = (dur > DHT11_BIT_HIGH_US) ? 1 : 0;
            } else {
                /*
                 * 最后一位后面没有下降沿(传感器直接释放总线)。
                 * '0' 的话必然会在 26us 后出现一个下降沿, 所以缺下降沿只可能是 '1'。
                 */
                bit = 1;
            }

            bits[i]  = bit;
            nbits    = i + 1;
        }

        if (bad || nbits < 39) {
            continue;
        }

        uint8_t d[5] = {0};
        for (int i = 0; i < nbits; i++) {
            d[i / 8] |= (bits[i] & 1) << (7 - (i % 8));
        }

        uint8_t expected = (uint8_t)(d[0] + d[1] + d[2] + d[3]);

        if (nbits == 40) {
            if (d[4] != expected) {
                continue;                       /* 校验和不符, 换下一个起点 */
            }
        } else {
            /* 只解出 39 位: 校验和的高 7 位必须对得上, 最低位由校验和补全 */
            if ((d[4] >> 1) != (expected >> 1)) {
                continue;
            }
            d[4] = expected;
        }

        memcpy(data, d, sizeof(d));
        return true;
    }

    return false;
}

/**
 * @brief 读一次原始 5 字节
 */
static esp_err_t dht11_read_raw(uint8_t data[5])
{
    /* Phase A: 拉低总线 >= 18ms
     *
     * 顺序很重要: 先把输出锁存器置 0, 再切到输出, 这样不会有"先高后低"的毛刺;
     * 上拉则提前打开, 等切回输入时总线立刻就是高电平, 中间没有悬空窗口
     * (悬空的那几十微秒里, 传感器可能以为主机还没释放总线)。 */
    gpio_set_level(s_pin, 0);
    gpio_set_direction(s_pin, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(s_pin, GPIO_PULLUP_ENABLE);
    dht11_delay_ms(DHT11_START_LOW_MS);        /* 精确 20ms, 保证 >= 18ms */

    /* Phase B: 释放总线并开始抓边沿 */
    s_edge_count = 0;
    s_capturing  = true;
    gpio_set_direction(s_pin, GPIO_MODE_INPUT);

    esp_err_t err = dht11_wait_frame();
    s_capturing = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no response: only %u edges captured "
                      "(check wiring/pull-up: DATA->GPIO5, need a 10K pull-up)",
                 (unsigned)s_edge_count);
        return err;
    }

    /* 取快照再解码, 避免解码期间 ISR 还在写 s_edges */
    uint32_t n = s_edge_count;
    if (n > DHT11_MAX_EDGES) {
        n = DHT11_MAX_EDGES;
    }
    memcpy(s_snapshot, s_edges, n * sizeof(dht11_edge_t));

    if (!dht11_decode(s_snapshot, n, data)) {
        ESP_LOGW(TAG, "decode failed");
        dht11_dump_edges(s_snapshot, n);       /* 把波形打出来方便定位 */
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t dht11_read(dht11_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t raw[5] = {0};
        err = dht11_read_raw(raw);
        if (err == ESP_OK) {
            memcpy(out->raw, raw, sizeof(raw));

            /*
             * DHT11: b0=湿度整数, b1=湿度小数, b2=温度整数, b3=温度小数
             * DHT22/AM2302(和 DHT11 引脚兼容, 帧格式也兼容): 前两字节拼成
             * 湿度整数, 后两字节拼成温度整数, 都放大了 10 倍。
             * 用「湿度整数 > 100」来区分: DHT11 的湿度不会超过 100。
             */
            if (raw[0] > 100) {
                out->dht22       = true;
                out->humidity    = ((float)raw[0] * 256.0f + (float)raw[1]) * 0.1f;
                out->temperature = (((float)(raw[2] & 0x7F) * 256.0f + (float)raw[3]) * 0.1f);
            } else {
                out->humidity    = (float)raw[0] + (float)raw[1] * 0.1f;
                out->temperature = (float)raw[2] + (float)raw[3] * 0.1f;
            }

            out->valid = true;
            return ESP_OK;
        }

        /* 失败后别马上重试: DHT11 一次转换没做完时, 紧接着再读容易继续失败 */
        dht11_delay_ms(100);
    }

    out->valid = false;
    ESP_LOGW(TAG, "read failed after 3 attempts: %s", esp_err_to_name(err));
    return err;
}
