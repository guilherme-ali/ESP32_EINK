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

typedef struct audio_codec_ctrl_if_t audio_codec_ctrl_if_t;

struct audio_codec_ctrl_if_t {
    int (*open)(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size);
    bool (*is_open)(const audio_codec_ctrl_if_t *ctrl);
    int (*read_reg)(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len);
    int (*write_reg)(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len);
    int (*close)(const audio_codec_ctrl_if_t *ctrl);
};

#ifdef __cplusplus
}
#endif
