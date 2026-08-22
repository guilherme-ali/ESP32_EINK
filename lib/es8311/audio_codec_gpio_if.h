/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Portado de esp_codec_dev (ver audio_codec_if.h para contexto).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  AUDIO_GPIO_MODE_FLOAT,
  AUDIO_GPIO_MODE_PULL_UP = (1 << 0),
  AUDIO_GPIO_MODE_PULL_DOWN = (1 << 1),
} audio_gpio_mode_t;

typedef enum {
  AUDIO_GPIO_DIR_OUT,
  AUDIO_GPIO_DIR_IN,
} audio_gpio_dir_t;

typedef struct {
  int (*setup)(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t mode);
  int (*set)(int16_t gpio, bool high);
  bool (*get)(int16_t gpio);
} audio_codec_gpio_if_t;

#ifdef __cplusplus
}
#endif
