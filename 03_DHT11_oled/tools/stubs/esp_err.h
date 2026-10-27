/* ===========================================================================
 * 主机端(stub)头文件 —— 只为在 PC 上编译/测试 dht11.c 的解码逻辑
 * 这些头不是 ESP-IDF 的, 只提供最小 API 声明, 让 gcc 能过编译。
 * ===========================================================================*/
#pragma once
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x108

const char *esp_err_to_name(esp_err_t code);

#define ESP_ERROR_CHECK(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { return _e; } } while (0)
