/**
 * @file    main.c
 * @brief   03_DHT11_oled —— DHT11 温湿度 + SSD1306 OLED 显示
 *
 * 结构 (用户选定: 独立 task + 队列):
 *
 *   dht_task (prio 5)                display_task (prio 4)
 *   ------------------               ------------------
 *   每 2 秒读一次 DHT11              阻塞等队列
 *        |                                   ^
 *        v                                   |
 *   xQueueSend(dht11_data_t)  ------->  xQueueReceive()
 *
 * 为什么这么分:
 *   - DHT11 是单总线时序协议, 读一次要 ~25ms 且有最小采样周期, 不能被显示任务拖住;
 *   - OLED 刷新是 SPI 事务, 走另一个任务后两者互不阻塞;
 *   - 队列做「生产者-消费者」, 读到的数据和显示节奏解耦: 即使 DHT 偶尔读失败,
 *     显示任务仍然能显示上一次的正常值或错误提示, 不会卡死。
 *
 * OLED 只由 display_task 访问(驱动的锚点表不是线程安全的), 不要在别处调用 OLED_xxx。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "LED.h"
#include "OLED.h"
#include "dht11.h"

static const char *TAG = "app";

/* DHT11 数据引脚 */
#define DHT11_PIN        GPIO_NUM_5

/* 采样周期: DHT11 手册要求至少 1 秒, 这里取 2 秒留余量 */
#define DHT11_PERIOD_MS  2000

/* 队列深度: 攒 4 帧, 极端情况下显示任务卡住也不丢数据 */
#define DATA_QUEUE_LEN   4

/* 显示区域: 8x16 字号, 每行 12 个字符 = 96 像素 */
#define LINE_Y0          0       /* 标题 */
#define LINE_Y1          16      /* 温度 */
#define LINE_Y2          32      /* 湿度 */
#define LINE_Y3          48      /* 状态 */

#define OLED_LINE_CHARS  12

/**
 * @brief  DHT 采集任务: 周期性读传感器, 把结果丢进队列
 */
static void dht_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    dht11_data_t data;

    ESP_LOGI(TAG, "dht task started, pin=GPIO%d, period=%dms",
             DHT11_PIN, DHT11_PERIOD_MS);

    /* 上电后传感器需要一点时间稳定, 先等一会儿再读第一次 */
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        esp_err_t err = dht11_read(&data);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "temp=%.1f C  humi=%.1f %% (%s)",
                     data.temperature, data.humidity,
                     data.dht22 ? "DHT22" : "DHT11");
            led_on();
        } else {
            data.valid = false;
            ESP_LOGW(TAG, "dht read failed: %s", esp_err_to_name(err));
            led_off();
        }

        /* 队列满说明显示任务跟不上, 丢最旧的一帧, 保证新数据能进去 */
        if (xQueueSend(queue, &data, 0) != pdTRUE) {
            dht11_data_t discard;
            xQueueReceive(queue, &discard, 0);
            xQueueSend(queue, &data, 0);
            ESP_LOGW(TAG, "queue full, dropped oldest sample");
        }

        vTaskDelay(pdMS_TO_TICKS(DHT11_PERIOD_MS));
    }
}

/**
 * @brief  显示任务: 从队列取数据刷新 OLED
 */
static void display_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    dht11_data_t data;
    char line[OLED_LINE_CHARS + 1];

    /* 先画静态标题 */
    OLED_ShowString(0, LINE_Y0, "DHT11", 16);

    while (1) {
        if (xQueueReceive(queue, &data, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (data.valid) {
            snprintf(line, sizeof(line), "Temp:%5.1fC", data.temperature);
            OLED_ShowString(0, LINE_Y1, line, 16);

            snprintf(line, sizeof(line), "Humi:%5.1f%%", data.humidity);
            OLED_ShowString(0, LINE_Y2, line, 16);

            OLED_ShowString(0, LINE_Y3, "Status:  OK", 16);
        } else {
            /* 读失败: 把前两行清成横杠, 表示数据无效(而不是显示旧值冒充新值) */
            OLED_ShowString(0, LINE_Y1, "Temp:  ---.-C", 16);
            OLED_ShowString(0, LINE_Y2, "Humi:  ---.-%", 16);

            OLED_ShowString(0, LINE_Y3, "Status: ERR", 16);
        }
    }
}

void app_main(void)
{
    led_init();
    led_off();               /* 先给一个确定的初始状态: 灭 */

    ESP_ERROR_CHECK(dht11_init(DHT11_PIN));

    OLED_Init();

    QueueHandle_t queue = xQueueCreate(DATA_QUEUE_LEN, sizeof(dht11_data_t));
    if (queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    /* 采集任务优先级高一点: 保证 DHT 的采样间隔和时序不受显示任务影响 */
    if (xTaskCreate(dht_task, "dht_task", 4096, queue, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dht_task create failed");
        return;
    }
    if (xTaskCreate(display_task, "display_task", 4096, queue, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "display_task create failed");
        return;
    }

    ESP_LOGI(TAG, "app_main done");
}
