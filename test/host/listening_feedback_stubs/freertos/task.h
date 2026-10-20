#pragma once
#include "FreeRTOS.h"
struct FeedbackTask;
using TaskHandle_t = FeedbackTask *;
using TaskFunction_t = void (*)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name, uint32_t stack,
                                 void *argument, UBaseType_t priority, TaskHandle_t *task,
                                 BaseType_t core);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
