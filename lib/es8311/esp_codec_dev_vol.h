/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Portado de esp_codec_dev (ver audio_codec_if.h para contexto).
 */
#pragma once
#include "esp_codec_dev_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int vol;
  float db_value;
} esp_codec_dev_vol_map_t;

typedef struct {
  esp_codec_dev_vol_map_t min_vol;
  esp_codec_dev_vol_map_t max_vol;
} esp_codec_dev_vol_range_t;

typedef struct {
  float pa_voltage;
  float codec_dac_voltage;
  float pa_gain;
} esp_codec_dev_hw_gain_t;

int esp_codec_dev_vol_calc_reg(const esp_codec_dev_vol_range_t *vol_range, float db);
float esp_codec_dev_vol_calc_db(const esp_codec_dev_vol_range_t *vol_range, int vol);
float esp_codec_dev_col_calc_hw_gain(esp_codec_dev_hw_gain_t *hw_gain);

#ifdef __cplusplus
}
#endif
