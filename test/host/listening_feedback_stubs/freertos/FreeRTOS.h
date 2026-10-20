#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
// Exercise the driver's minimum-one-tick guard as well as finish yielding.
#define pdMS_TO_TICKS(ms) ((ms) / 10)
