#pragma once
#include <freertos/FreeRTOS.h>

// recorder.cpp usa AudioCodec real no contrato, substituindo somente read()
// na TU do teste. O driver de hardware nao participa do executavel host.
enum i2s_port_t { I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_NUM_MAX = 2 };
