/* 主机端 stub: freertos/task.h */
#pragma once
#include "freertos/FreeRTOS.h"

void       vTaskDelay(TickType_t ticks);
void       taskYIELD(void);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned prio, void *handle);
