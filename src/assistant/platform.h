#pragma once
#ifdef ASSISTANT_HOST_TEST
#include "assistant_host_platform.h"
#else
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha1.h>
#include "../audio/codec.h"
#include "../net/settings.h"
#include "../net/network_client.h"
#include "../net/http_client.h"
#include "../connectivity/network_route.h"
inline uint64_t assistantMonotonicUs() { return static_cast<uint64_t>(esp_timer_get_time()); }
#endif
