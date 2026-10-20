#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using TickType_t = uint32_t;
// 100 Hz exercises the minimum-one-tick BUSY polling guard.
#define pdMS_TO_TICKS(ms) ((ms) / 10)
#define PROGMEM
uint32_t millis();
void vTaskDelay(TickType_t ticks);
