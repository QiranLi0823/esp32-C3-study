/* 主机端 stub: freertos/FreeRTOS.h */
#pragma once
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define pdTRUE              1
#define pdFALSE             0
#define pdPASS              1
#define pdFAIL              0
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xffffffffUL)
