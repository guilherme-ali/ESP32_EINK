#include "codec.h"
#include "../config/pins.h"
#include <Wire.h>

extern "C" {
#include "es8311_codec.h"
}

namespace {

// 0x30 e o "default" do driver vendor, mas o scan I2C desta placa mostra
// o ES8311 respondendo em 0x18 (mesma estrapagem do ESP32_S3_BOX_3 no
// board_cfg.txt oficial).
constexpr uint8_t ES8311_I2C_ADDR = 0x18;

// --- ctrl_if: registrador do ES8311 via I2C (Wire) ---
bool ctrlIsOpenImpl(const audio_codec_ctrl_if_t *) { return true; }

int ctrlWriteReg(const audio_codec_ctrl_if_t *, int reg, int regLen, void *data, int dataLen) {
  (void)regLen;
  Wire.beginTransmission(ES8311_I2C_ADDR);
  Wire.write((uint8_t)reg);
  Wire.write((uint8_t *)data, dataLen);
  return Wire.endTransmission() == 0 ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

int ctrlReadReg(const audio_codec_ctrl_if_t *, int reg, int regLen, void *data, int dataLen) {
  (void)regLen;
  Wire.beginTransmission(ES8311_I2C_ADDR);
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0) return ESP_CODEC_DEV_READ_FAIL;
  int got = Wire.requestFrom((int)ES8311_I2C_ADDR, dataLen);
  if (got != dataLen) return ESP_CODEC_DEV_READ_FAIL;
  uint8_t *out = (uint8_t *)data;
  for (int i = 0; i < dataLen; i++) out[i] = Wire.read();
  return ESP_CODEC_DEV_OK;
}

audio_codec_ctrl_if_t g_ctrlIf = {
    .open = nullptr,
    .is_open = ctrlIsOpenImpl,
    .read_reg = ctrlReadReg,
    .write_reg = ctrlWriteReg,
    .close = nullptr,
};

// --- gpio_if: pino de enable do amplificador (PA), controlado pelo driver ---
int gpioSetup(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t) {
  pinMode(gpio, dir == AUDIO_GPIO_DIR_OUT ? OUTPUT : INPUT);
  return ESP_CODEC_DEV_OK;
}

int gpioSet(int16_t gpio, bool high) {
  digitalWrite(gpio, high ? HIGH : LOW);
  return ESP_CODEC_DEV_OK;
}

bool gpioGet(int16_t gpio) { return digitalRead(gpio) == HIGH; }

audio_codec_gpio_if_t g_gpioIf = {
    .setup = gpioSetup,
    .set = gpioSet,
    .get = gpioGet,
};

} // namespace

bool AudioCodec::begin() {
  es8311_codec_cfg_t cfg = {};
  cfg.ctrl_if = &g_ctrlIf;
  cfg.gpio_if = &g_gpioIf;
  cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
  cfg.pa_pin = PIN_AUDIO_PA;
  cfg.pa_reverted = false;
  cfg.master_mode = false; // ESP32 e o mestre I2S; ES8311 e escravo
  cfg.use_mclk = true;
  cfg.digital_mic = false;
  cfg.invert_mclk = false;
  cfg.invert_sclk = false;
  cfg.no_dac_ref = false;
  cfg.mclk_div = 0; // default MCLK_DEFAULT_DIV (256) dentro do driver
  cfg.hw_gain.pa_gain = 6.0f; // "pa_gain:6" no board_cfg.txt oficial

  const audio_codec_if_t *codec = es8311_codec_new(&cfg);
  codec_ = codec;
  if (!codec_) return false;

  // O driver vendor nunca escreve ES8311_ADC_REG18 (ALC enable) - o
  // default de fabrica pode deixar o AGC/automute ligado, o que corta
  // ou abafa trechos baixos da fala. Desabilita explicitamente.
  codec->set_reg(codec, 0x18, 0x00);
  return true;
}

bool AudioCodec::initI2s(uint32_t sampleRate) {
  if (i2sInited_) {
    i2s_driver_uninstall(kPort);
    i2sInited_ = false;
  }

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  i2sConfig.sample_rate = sampleRate;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  // 8 buffers de 512 frames ~= 341ms de audio a 12kHz de folga - o
  // refresh do e-paper e outras pausas da task de UI (core 1) nao devem
  // mais fazer o DMA estourar (a captura agora roda em task propria no
  // core 0, ver audio/recorder.cpp).
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = 512;
  i2sConfig.use_apll = true; // APLL da menos erro de clock, ES8311 e sensivel a isso

  if (i2s_driver_install(kPort, &i2sConfig, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = PIN_I2S_MCLK;
  pinConfig.bck_io_num = PIN_I2S_BCLK;
  pinConfig.ws_io_num = PIN_I2S_WS;
  pinConfig.data_out_num = PIN_I2S_DOUT;
  pinConfig.data_in_num = PIN_I2S_DIN;

  if (i2s_set_pin(kPort, &pinConfig) != ESP_OK) return false;

  i2sInited_ = true;
  return true;
}

bool AudioCodec::setSampleRate(uint32_t sampleRate) {
  if (!codec_) return false;
  if (!initI2s(sampleRate)) return false;

  auto *codec = (const audio_codec_if_t *)codec_;
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = sampleRate;
  fs.channel = 2;
  fs.bits_per_sample = 16;
  return codec->set_fs(codec, &fs) == ESP_CODEC_DEV_OK;
}

bool AudioCodec::enable(bool on) {
  if (!codec_) return false;
  auto *codec = (const audio_codec_if_t *)codec_;
  codecOpen_ = on;
  return codec->enable(codec, on) == ESP_CODEC_DEV_OK;
}

bool AudioCodec::setVolume(float db) {
  if (!codec_) return false;
  auto *codec = (const audio_codec_if_t *)codec_;
  return codec->set_vol(codec, db) == ESP_CODEC_DEV_OK;
}

bool AudioCodec::setMicGain(float db) {
  if (!codec_) return false;
  auto *codec = (const audio_codec_if_t *)codec_;
  return codec->set_mic_gain(codec, db) == ESP_CODEC_DEV_OK;
}

bool AudioCodec::read(void *buf, size_t len, size_t *bytesRead, uint32_t timeoutMs) {
  if (!i2sInited_) return false;
  return i2s_read(kPort, buf, len, bytesRead, pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
}

bool AudioCodec::write(const void *buf, size_t len, size_t *bytesWritten, uint32_t timeoutMs) {
  if (!i2sInited_) return false;
  return i2s_write(kPort, buf, len, bytesWritten, pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
}
