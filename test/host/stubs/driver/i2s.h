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
#define I2S_PIN_NO_CHANGE -1

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

#ifdef AUDIO_CODEC_TEST_HOOKS
#include <cstring>
namespace Host {
struct I2sState {
  bool installSuccess = true, pinSuccess = true, clearSuccess = true, uninstallSuccess = true;
  bool writeSuccess = true, readSuccess = true, installed = false;
  size_t writeLimit = static_cast<size_t>(-1), totalWritten = 0;
  size_t readLimit = static_cast<size_t>(-1), totalRead = 0;
  size_t failWriteAfter = static_cast<size_t>(-1);
  bool oversizedWrite = false;
  unsigned installs = 0, uninstalls = 0, clears = 0;
  void (*writeHook)(const void *, size_t) = nullptr;
  i2s_config_t config = {};
  i2s_pin_config_t pins = {};
};
inline I2sState i2s;
}
inline esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t *cfg, int, void *) {
  ++Host::i2s.installs;
  if (!Host::i2s.installSuccess || Host::i2s.installed) return ESP_FAIL;
  Host::i2s.config = *cfg;
  Host::i2s.installed = true;
  return ESP_OK;
}
inline esp_err_t i2s_set_pin(i2s_port_t, const i2s_pin_config_t *pins) {
  Host::i2s.pins = *pins;
  return Host::i2s.pinSuccess ? ESP_OK : ESP_FAIL;
}
inline esp_err_t i2s_zero_dma_buffer(i2s_port_t) {
  ++Host::i2s.clears;
  return Host::i2s.clearSuccess ? ESP_OK : ESP_FAIL;
}
inline esp_err_t i2s_driver_uninstall(i2s_port_t) {
  ++Host::i2s.uninstalls;
  if (!Host::i2s.uninstallSuccess) return ESP_FAIL;
  Host::i2s.installed = false;
  return ESP_OK;
}
inline esp_err_t i2s_read(i2s_port_t, void *buf, size_t len, size_t *read, TickType_t) {
  size_t n = len < Host::i2s.readLimit ? len : Host::i2s.readLimit;
  if (!Host::i2s.readSuccess || !Host::i2s.installed) n = 0;
  if (read) *read = n;
  std::memset(buf, 0, n);
  Host::i2s.totalRead += n;
  return Host::i2s.readSuccess && Host::i2s.installed ? ESP_OK : ESP_FAIL;
}
inline esp_err_t i2s_write(i2s_port_t, const void *data, size_t len, size_t *written, TickType_t) {
  size_t n = len < Host::i2s.writeLimit ? len : Host::i2s.writeLimit;
  const bool ok = Host::i2s.writeSuccess && Host::i2s.installed &&
                  Host::i2s.totalWritten < Host::i2s.failWriteAfter;
  if (!ok) n = 0;
  if (Host::i2s.oversizedWrite) n = len + 1;
  if (written) *written = n;
  Host::i2s.totalWritten += n;
  if (Host::i2s.writeHook && n <= len) Host::i2s.writeHook(data, n);
  return ok ? ESP_OK : ESP_FAIL;
}
#else
inline esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t *, int, void *) { return ESP_OK; }
inline esp_err_t i2s_set_pin(i2s_port_t, const i2s_pin_config_t *) { return ESP_OK; }
inline esp_err_t i2s_zero_dma_buffer(i2s_port_t) { return ESP_OK; }
inline esp_err_t i2s_driver_uninstall(i2s_port_t) { return ESP_OK; }
inline esp_err_t i2s_read(i2s_port_t, void *, size_t, size_t *read, TickType_t) { if (read) *read = 0; return ESP_OK; }
inline esp_err_t i2s_write(i2s_port_t, const void *, size_t, size_t *written, TickType_t) { if (written) *written = 0; return ESP_OK; }
#endif
