#pragma once
#include <freertos/FreeRTOS.h>
#include <esp_err.h>
#include <cstdint>
#include <cstddef>

enum i2s_port_t { I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_NUM_MAX = 2 };

typedef int i2s_mode_t;
#define I2S_MODE_MASTER 1
#define I2S_MODE_TX     2
#define I2S_MODE_RX     4

typedef int i2s_bits_per_sample_t;
#define I2S_BITS_PER_SAMPLE_16BIT 16

typedef int i2s_channel_fmt_t;
#define I2S_CHANNEL_FMT_RIGHT_LEFT 1

typedef int i2s_comm_format_t;
#define I2S_COMM_FORMAT_STAND_I2S 1

#define ESP_INTR_FLAG_LEVEL1 0

struct i2s_config_t {
  i2s_mode_t mode;
  uint32_t sample_rate;
  i2s_bits_per_sample_t bits_per_sample;
  i2s_channel_fmt_t channel_format;
  i2s_comm_format_t communication_format;
  int intr_alloc_flags;
  int dma_buf_count;
  int dma_buf_len;
  bool use_apll;
  bool tx_desc_auto_clear;
};

struct i2s_pin_config_t {
  int mck_io_num;
  int bck_io_num;
  int ws_io_num;
  int data_out_num;
  int data_in_num;
};

inline esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t *, int, void *) { return ESP_OK; }
inline esp_err_t i2s_set_pin(i2s_port_t, const i2s_pin_config_t *) { return ESP_OK; }
inline esp_err_t i2s_driver_uninstall(i2s_port_t) { return ESP_OK; }
inline esp_err_t i2s_read(i2s_port_t, void *, size_t, size_t *read, TickType_t) { if (read) *read = 0; return ESP_OK; }
inline esp_err_t i2s_write(i2s_port_t, const void *, size_t, size_t *written, TickType_t) { if (written) *written = 0; return ESP_OK; }
