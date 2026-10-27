#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

void app_main(void)
{
    while(1){
        ESP_LOGI("main", "hello world\r\n");
        ESP_LOGE("main", "hello world\r\n");

        printf("hello world 123\r\n");
        vTaskDelay(500);
    }
}
