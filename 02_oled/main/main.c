#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "LED.h"
#include "OLED.h"

/* 状态文字在屏幕上的位置: 8x16 字号, 每行 12 个字符 = 96 像素 */
// #define STATUS_X  16
// #define STATUS_Y  24

void app_main(void)
{
    led_init();
    OLED_Init();

    // 显示数字
    OLED_ShowNum(0, 0, 20000823, 8, 16);

    while(1){
        led_on();
        printf("led is on...\n");
        OLED_ShowString(0, 24, "led is on...", 16);
        vTaskDelay(pdMS_TO_TICKS(500));

        led_off();
        printf("led is off...\n");
        OLED_ShowString(0, 24, "led is off...", 16);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
