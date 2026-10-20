#include "codec.h"
#include "../config/pins.h"
#include <Wire.h>
#include <cmath>
#include <cstdlib>

extern "C" {
#include "es8311_codec.h"
}

namespace {

// 0x30 e o "default" do driver vendor, mas o scan I2C desta placa mostra
// o ES8311 respondendo em 0x18 (mesma estrapagem do ESP32_S3_BOX_3 no
// board_cfg.txt oficial).
constexpr uint8_t ES8311_I2C_ADDR = 0x18;
constexpr float kCalibratedMaxBoostDb = 4.0f;

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
  return codec_ ? true : open(Mode::Legacy);
}

bool AudioCodec::open(Mode mode) {
  // Preload output latches before enabling GPIOs: no pulse on PA or rail.
  digitalWrite(PIN_AUDIO_PA, LOW);
  pinMode(PIN_AUDIO_PA, OUTPUT);
  digitalWrite(PIN_AUDIO_PWR, LOW);
  pinMode(PIN_AUDIO_PWR, OUTPUT);
  // Preserve the board's proven rail-settling interval after a real power-on.
  delay(10);
  mode_ = mode;
  // Arduino-ESP32 returns true without reinitializing an already started bus.
  if (!Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL)) {
    end();
    return false;
  }
  es8311_codec_cfg_t cfg = {};
  cfg.ctrl_if = &g_ctrlIf;
  cfg.gpio_if = &g_gpioIf;
  cfg.codec_mode = mode == Mode::Capture ? ESP_CODEC_DEV_WORK_MODE_ADC :
                   mode == Mode::Playback ? ESP_CODEC_DEV_WORK_MODE_DAC : ESP_CODEC_DEV_WORK_MODE_BOTH;
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  if (mode == Mode::Playback && playbackBoth_) cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
#endif
  // The wrapper raises PA only after clocks, mute and volume are restored.
  cfg.pa_pin = -1;
  cfg.pa_reverted = false;
  cfg.master_mode = false; // ESP32 e o mestre I2S; ES8311 e escravo
  cfg.use_mclk = true;
  cfg.digital_mic = false;
  cfg.invert_mclk = false;
  cfg.invert_sclk = false;
  cfg.no_dac_ref = mode == Mode::Capture;
  cfg.mclk_div = 0; // default MCLK_DEFAULT_DIV (256) dentro do driver
  cfg.hw_gain.pa_gain = 6.0f; // "pa_gain:6" no board_cfg.txt oficial

  const audio_codec_if_t *codec = es8311_codec_new(&cfg);
  codec_ = codec;
  if (!codec_) {
    end();
    return false;
  }

  // O driver vendor nunca escreve ES8311_ADC_REG18 (ALC enable) - o
  // default de fabrica pode deixar o AGC/automute ligado, o que corta
  // ou abafa trechos baixos da fala. Desabilita explicitamente.
  if (codec->set_reg(codec, 0x18, 0x00) != ESP_CODEC_DEV_OK ||
      codec->mute(codec, true) != ESP_CODEC_DEV_OK ||
      codec->enable(codec, false) != ESP_CODEC_DEV_OK) {
    end();
    return false;
  }
  return true;
}

bool AudioCodec::stopI2s() {
  if (i2sInited_) {
    // Uninstall also stops I2S and releases its APLL reference and DMA buffers.
    if (i2s_driver_uninstall(kPort) != ESP_OK) return false;
    i2sInited_ = false;
  }
  return true;
}

void AudioCodec::end() {
  enable(false);
  if (codec_) {
    auto *codec = (const audio_codec_if_t *)codec_;
    codec->close(codec);
    // es8311_codec_new allocates one calloc block; close only suspends it.
    std::free(const_cast<void *>(codec_));
    codec_ = nullptr;
  }
  // Disconnect only audio signals, with no internal pullups into an unpowered IC.
  const int audioPins[] = {PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN};
  for (int pin : audioPins) {
    pinMode(pin, INPUT);
  }
  digitalWrite(PIN_AUDIO_PWR, HIGH);
  pinMode(PIN_AUDIO_PWR, OUTPUT);
  boundedSpeech_ = false;
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  playbackBoth_ = false;
  playbackSettleMs_ = 0;
  playbackTrimDb_ = 0;
#endif
}

bool AudioCodec::prepareCapture(uint32_t sampleRate, float micGainDb) {
  if (!std::isfinite(micGainDb) || !sampleRate) return false;
  if (codec_ && mode_ != Mode::Capture) end();
  if ((!codec_ && !open(Mode::Capture)) || !setSampleRate(sampleRate) ||
      !setMicGain(micGainDb) || !enable(true)) {
    end();
    return false;
  }
  return true;
}

bool AudioCodec::preparePlayback(uint32_t sampleRate, float percent, bool boundedSpeech) {
  if (!std::isfinite(percent) || percent < 0 || percent > 100 || !sampleRate) return false;
  if (codec_ && mode_ != Mode::Playback) end();
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  if (codec_ && (playbackBoth_ || playbackSettleMs_ || playbackTrimDb_ != 0)) end();
  playbackBoth_ = false;
  playbackSettleMs_ = 0;
  playbackTrimDb_ = 0;
#endif
  boundedSpeech_ = boundedSpeech;
  return startPlayback(sampleRate, percent);
}

#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
bool AudioCodec::preparePlaybackTest(uint32_t sampleRate, float percent, bool both, uint32_t settleMs,
                                     float trimDb) {
  if (settleMs > 1000 || !std::isfinite(percent) || percent < 0 || percent > 100 || !sampleRate ||
      !std::isfinite(trimDb) || trimDb < 0 || trimDb > 8) {
    end();
    return false;
  }
  if (codec_ && mode_ != Mode::Playback) end();
  if (codec_ && (playbackBoth_ != both || playbackSettleMs_ != settleMs || playbackTrimDb_ != trimDb)) end();
  playbackBoth_ = both;
  playbackSettleMs_ = settleMs;
  playbackTrimDb_ = trimDb;
  boundedSpeech_ = false;
  return startPlayback(sampleRate, percent);
}
#endif

bool AudioCodec::startPlayback(uint32_t sampleRate, float percent) {
  if ((!codec_ && !open(Mode::Playback)) || !setSampleRate(sampleRate) ||
      !setVolume(percent) || !enable(true)) {
    end();
    return false;
  }
  return true;
}

#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
bool AudioCodec::readRegister(uint8_t address, uint16_t &value) {
  if (!codec_) return false;
  auto *codec = (const audio_codec_if_t *)codec_;
  int actual = 0;
  if (!codec->get_reg || codec->get_reg(codec, address, &actual) != ESP_CODEC_DEV_OK) return false;
  value = static_cast<uint16_t>(actual);
  return true;
}
#endif

bool AudioCodec::initI2s(uint32_t sampleRate) {
  if (!stopI2s()) return false;

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER |
      (mode_ != Mode::Capture ? I2S_MODE_TX : 0) |
      (mode_ != Mode::Playback ? I2S_MODE_RX : 0));
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
  // Rede pode atrasar: underflow deve transmitir silencio, nao repetir o DMA antigo.
  i2sConfig.tx_desc_auto_clear = true;

  if (i2s_driver_install(kPort, &i2sConfig, 0, nullptr) != ESP_OK) return false;
  i2sInited_ = true; // Own it immediately, including failures configuring pins.

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = PIN_I2S_MCLK;
  pinConfig.bck_io_num = PIN_I2S_BCLK;
  pinConfig.ws_io_num = PIN_I2S_WS;
  pinConfig.data_out_num = mode_ != Mode::Capture ? PIN_I2S_DOUT : I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = mode_ != Mode::Playback ? PIN_I2S_DIN : I2S_PIN_NO_CHANGE;

  if (i2s_set_pin(kPort, &pinConfig) != ESP_OK ||
      (mode_ != Mode::Capture && i2s_zero_dma_buffer(kPort) != ESP_OK)) {
    stopI2s();
    return false;
  }

  return true;
}

bool AudioCodec::setSampleRate(uint32_t sampleRate) {
  if (!codec_) return false;
  if (sampleRate < 8000 || sampleRate > 96000) return false;
  if (sampleRate == sampleRate_ && i2sInited_) return true;
  const bool resume = codecOpen_;
  if (!enable(false) || !initI2s(sampleRate)) return false;

  auto *codec = (const audio_codec_if_t *)codec_;
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = sampleRate;
  fs.channel = 2;
  fs.bits_per_sample = 16;
  if (codec->set_fs(codec, &fs) != ESP_CODEC_DEV_OK) {
    enable(false);
    return false;
  }
  sampleRate_ = sampleRate;
  return !resume || enable(true);
}

bool AudioCodec::enable(bool on) {
  if (!on) {
    digitalWrite(PIN_AUDIO_PA, LOW);
    pinMode(PIN_AUDIO_PA, OUTPUT);
    codecOpen_ = false;
    bool suspended = true;
    if (codec_) {
      auto *codec = (const audio_codec_if_t *)codec_;
      suspended = codec->mute(codec, true) == ESP_CODEC_DEV_OK;
      suspended = (codec->enable(codec, false) == ESP_CODEC_DEV_OK) && suspended;
    }
    const bool stopped = stopI2s();
    return suspended && stopped;
  }
  if (!codec_) return false;
  if (codecOpen_) return true;
  digitalWrite(PIN_AUDIO_PA, LOW);
  auto *codec = (const audio_codec_if_t *)codec_;
  if ((!i2sInited_ && !setSampleRate(sampleRate_)) ||
      codec->mute(codec, true) != ESP_CODEC_DEV_OK ||
      codec->enable(codec, true) != ESP_CODEC_DEV_OK) {
    enable(false);
    return false;
  }
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  // Bias/reference settling is experimental, never a production default.
  if (mode_ == Mode::Playback && playbackSettleMs_) delay(playbackSettleMs_);
#endif
  codecOpen_ = true;
  // Suspend clears clock dividers and DAC volume, not just the mute bit.
  if (!setMicGain(micGainDb_) || !applyVolume()) {
    enable(false);
    return false;
  }
  digitalWrite(PIN_AUDIO_PA, mode_ != Mode::Capture && volumePercent_ > 0 ? HIGH : LOW);
  return true;
}

bool AudioCodec::setVolume(float percent) {
  if (!std::isfinite(percent) || percent < 0 || percent > 100) return false;
  const float previous = volumePercent_;
  volumePercent_ = percent;
  if (!codec_) return true;
  digitalWrite(PIN_AUDIO_PA, LOW);
  if (applyVolume()) {
    if (codecOpen_ && mode_ != Mode::Capture && percent > 0) digitalWrite(PIN_AUDIO_PA, HIGH);
    return true;
  }
  volumePercent_ = previous;
  return false;
}

bool AudioCodec::applyVolume() {
  if (!codec_) return false;
  auto *codec = (const audio_codec_if_t *)codec_;
  // Change gain while muted; never unmute after a failed register write.
  if (codec->mute(codec, true) != ESP_CODEC_DEV_OK) return false;
  if (volumePercent_ == 0 || mode_ == Mode::Capture) return true;
  float db = 40.0f * std::log10(volumePercent_ / 100.0f);
  if (mode_ == Mode::Playback) {
    if (boundedSpeech_) {
      if (volumePercent_ > 60) db += (volumePercent_ - 60) * kCalibratedMaxBoostDb / 40.0f;
    }
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
    else db += playbackTrimDb_;
#endif
  }
  if (codec->set_vol(codec, db) != ESP_CODEC_DEV_OK) return false;
  return codec->mute(codec, false) == ESP_CODEC_DEV_OK;
}

bool AudioCodec::setMicGain(float db) {
  if (!codec_ || !std::isfinite(db)) return false;
  auto *codec = (const audio_codec_if_t *)codec_;
  if (codec->set_mic_gain(codec, db) != ESP_CODEC_DEV_OK) return false;
  micGainDb_ = db;
  return true;
}

bool AudioCodec::read(void *buf, size_t len, size_t *bytesRead, uint32_t timeoutMs) {
  if (bytesRead) *bytesRead = 0;
  if (!i2sInited_ || !codecOpen_ || mode_ == Mode::Playback) return false;
  return i2s_read(kPort, buf, len, bytesRead, pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
}

bool AudioCodec::write(const void *buf, size_t len, size_t *bytesWritten, uint32_t timeoutMs) {
  if (bytesWritten) *bytesWritten = 0;
  if (!i2sInited_ || !codecOpen_ || mode_ == Mode::Capture) return false;
  return i2s_write(kPort, buf, len, bytesWritten, pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
}
