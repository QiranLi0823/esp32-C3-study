/**
 * @file    dht11.h
 * @brief   DHT11 单总线温湿度传感器驱动 —— ESP32-C3
 *
 * 接线 (ESP32-C3 SuperMini):
 *   VCC  -> 3V3
 *   DATA -> GPIO5   (开漏单总线, 必须有上拉电阻; 常见 DHT11 模块板上自带 10K)
 *   GND  -> GND
 *
 * 实现方式: GPIO 双边沿中断 + esp_timer_get_time() 记录边沿时间戳,
 *           在任务里根据脉宽解码 40 位数据。
 *           用中断而不是轮询, 是因为 '0'(26~28us 高电平) 和 '1'(70us 高电平)
 *           的差别很窄, 轮询一旦被 FreeRTOS 抢占就会读错;
 *           中断抓时间戳则完全不受任务调度影响。
 *
 * 时序:
 *   主机: 拉低 >= 18ms  ->  释放总线(上拉)
 *   从机: 拉低 ~80us (应答低)  ->  拉高 ~80us (应答高)  ->  40 位数据
 *   每位: 低 ~50us, 然后高电平 26~28us 表示 '0', 70us 表示 '1'
 *
 * 注意事项:
 *   - DHT11 最小采样周期约 1 秒(手册建议 2 秒), 不要连续快速读取;
 *   - 起始信号必须 >= 18ms, 驱动里用 esp_timer 精确定时(不能用 vTaskDelay);
 *   - 读失败时会自动重试 3 次, 整体阻塞约 300ms;
 *   - 读操作本身不应被长任务抢占, 建议放在较高优先级的任务里。
 */
#ifndef DHT11_H
#define DHT11_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** DHT11 读到的数据 */
typedef struct {
    float    temperature;    /* 温度, 单位 ℃ */
    float    humidity;       /* 湿度, 单位 %RH */
    uint8_t  raw[5];         /* 原始 5 字节: 湿度整数/湿度小数/温度整数/温度小数/校验和 */
    bool     valid;          /* true: 数据通过校验; false: 读取失败 */
    bool     dht22;          /* true: 实际是 DHT22/AM2302(帧格式兼容, 已按 DHT22 解析) */
} dht11_data_t;

/**
 * @brief  初始化 DHT11 数据引脚
 * @param  gpio_num  数据引脚号 (本工程用 GPIO5)
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t dht11_init(gpio_num_t gpio_num);

/**
 * @brief  读取一次温湿度 (阻塞约 40ms, 内部最多重试 3 次)
 * @param  out  输出数据, 出错时 out->valid = false
 * @return ESP_OK / ESP_ERR_TIMEOUT / ESP_ERR_INVALID_RESPONSE
 */
esp_err_t dht11_read(dht11_data_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DHT11_H */
