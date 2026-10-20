#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define PROGMEM
uint32_t millis();
uint32_t micros();
struct FeedbackSerial {
  void println(const char *message);
  void printf(const char *format, ...);
  size_t write(const uint8_t *data, size_t size);
};
extern FeedbackSerial Serial;
