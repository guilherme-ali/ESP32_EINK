#pragma once
#include <cstddef>
#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
using StackType_t = uint32_t;
using TaskFunction_t = void (*)(void *);
namespace HostRTOS { struct Task; struct StreamBuffer; }
using TaskHandle_t = HostRTOS::Task *;
using StreamBufferHandle_t = HostRTOS::StreamBuffer *;
struct StaticStreamBuffer_t { StreamBufferHandle_t handle; };
constexpr BaseType_t pdPASS = 1, pdFAIL = 0, pdTRUE = 1, pdFALSE = 0;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr TickType_t portTICK_PERIOD_MS = 1;
constexpr UBaseType_t tskIDLE_PRIORITY = 0;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
