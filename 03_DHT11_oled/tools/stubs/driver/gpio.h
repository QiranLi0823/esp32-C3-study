/* 主机端 stub: driver/gpio.h —— 只声明用到的 API */
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8

typedef enum {
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT,
} gpio_mode_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE,
} gpio_int_type_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE,
} gpio_pull_mode_t;

typedef gpio_pull_mode_t gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE,
} gpio_pulldown_t;

typedef struct {
    uint64_t      pin_bit_mask;
    gpio_mode_t   mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *cfg);
int       gpio_get_level(gpio_num_t g);
esp_err_t gpio_set_level(gpio_num_t g, uint32_t level);
esp_err_t gpio_set_direction(gpio_num_t g, gpio_mode_t mode);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t g, void (*handler)(void *), void *arg);
esp_err_t gpio_set_pull_mode(gpio_num_t g, gpio_pull_mode_t pull);
