#include <stdio.h>
#include "LED.h"
#include "driver/gpio.h"

void led_init(){
    esp_err_t error;
    gpio_config_t gpio_cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = 1ull << GPIO_NUM_8,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    error = gpio_config(&gpio_cfg);
    if(error!=ESP_OK){
        printf("gpio_config is fail...\r\n");
    }
}

void led_on(){
    gpio_set_level(GPIO_NUM_8, 1);
}

void led_off(){
    gpio_set_level(GPIO_NUM_8, 0);
}
