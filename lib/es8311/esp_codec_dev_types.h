/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Portado de esp_codec_dev (ver audio_codec_if.h para contexto).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_CODEC_DEV_OK          (0)
#define ESP_CODEC_DEV_DRV_ERR     (ESP_FAIL)
#define ESP_CODEC_DEV_INVALID_ARG (ESP_ERR_INVALID_ARG)
#define ESP_CODEC_DEV_NO_MEM      (ESP_ERR_NO_MEM)
#define ESP_CODEC_DEV_NOT_SUPPORT (ESP_ERR_NOT_SUPPORTED)
#define ESP_CODEC_DEV_NOT_FOUND   (ESP_ERR_NOT_FOUND)
#define ESP_CODEC_DEV_WRONG_STATE (ESP_ERR_INVALID_STATE)
#define ESP_CODEC_DEV_WRITE_FAIL  (0x10D)
#define ESP_CODEC_DEV_READ_FAIL   (0x10E)

typedef struct {
  uint8_t bits_per_sample;
  uint8_t channel;
  uint16_t channel_mask;
  uint32_t sample_rate;
  int mclk_multiple;
} esp_codec_dev_sample_info_t;

typedef enum {
  ESP_CODEC_DEV_WORK_MODE_NONE,
  ESP_CODEC_DEV_WORK_MODE_ADC = (1 << 0),
  ESP_CODEC_DEV_WORK_MODE_DAC = (1 << 1),
  ESP_CODEC_DEV_WORK_MODE_BOTH = (ESP_CODEC_DEV_WORK_MODE_ADC | ESP_CODEC_DEV_WORK_MODE_DAC),
  ESP_CODEC_DEV_WORK_MODE_LINE = (1 << 2),
} esp_codec_dec_work_mode_t;

#ifdef __cplusplus
}
#endif
