#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "driver/gpio.h"

#include "led.h"

void app_main(void)
{
    led_init();

    while(1){
        led_on();
        printf("led is on...\n");
        vTaskDelay(500);
        led_off();
        printf("led is off...\n");
        vTaskDelay(500);
    }
}
