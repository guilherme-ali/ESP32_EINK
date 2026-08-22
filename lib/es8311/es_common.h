/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Portado de esp_codec_dev (ver audio_codec_if.h) - so as constantes/enums
 * usados por es8311.c.
 */
#pragma once
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEC_MEM_CHECK(ptr)                                                 \
  if (ptr == NULL) {                                                         \
    ESP_LOGE(TAG, "Fail to alloc memory at %s:%d", __FUNCTION__, __LINE__);  \
  }

#define BITS(n) (1 << n)
#define MCLK_DEFAULT_DIV (256)

typedef enum {
  ES_I2S_MIN = -1,
  ES_I2S_NORMAL = 0,
  ES_I2S_LEFT = 1,
  ES_I2S_RIGHT = 2,
  ES_I2S_DSP = 3,
  ES_I2S_MAX
} es_i2s_fmt_t;

typedef enum {
  ES_PA_SETUP = 1,
  ES_PA_ENABLE = (1 << 1),
  ES_PA_DISABLE = (1 << 2),
} es_pa_setting_t;

#ifdef __cplusplus
}
#endif
