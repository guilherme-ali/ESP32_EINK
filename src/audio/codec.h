#pragma once
#include <Arduino.h>
#include <driver/i2s.h>

// Camada fina sobre o driver ES8311 (lib/es8311, portado do vendor) mais
// I2S proprio via ESP-IDF (API legada driver/i2s.h - o framework Arduino-
// ESP32 instalado e IDF 4.4, sem o driver i2s_std novo). Cobre so o que
// o projeto precisa: abrir o codec numa taxa de amostragem fixa,
// ligar/desligar, gravar e tocar PCM 16-bit estereo (o mono da gravacao
// final e extraido depois, em audio/recorder.cpp, pegando so o canal
// esquerdo).
//
// Pressupoe que o barramento I2C (Wire) ja foi iniciado por outro
// modulo (ver board/rtc.h) - RTC e codec dividem o mesmo barramento.
class AudioCodec {
public:
  bool begin();

  // sampleRate deve ter uma entrada na tabela de coeficientes do ES8311
  // (8000, 16000, 32000, 44100, 48000, ...) com mclk = sampleRate*256.
  bool setSampleRate(uint32_t sampleRate);

  bool enable(bool on);
  bool setVolume(float db);
  bool setMicGain(float db);

  // len em bytes; buffers sao PCM 16-bit estereo intercalado (LRLR...).
  bool read(void *buf, size_t len, size_t *bytesRead, uint32_t timeoutMs = 1000);
  bool write(const void *buf, size_t len, size_t *bytesWritten, uint32_t timeoutMs = 1000);

private:
  static constexpr i2s_port_t kPort = I2S_NUM_0;

  bool i2sInited_ = false;
  bool codecOpen_ = false;
  const void *codec_ = nullptr; // audio_codec_if_t* (tipo C, evita expor o header aqui)

  bool initI2s(uint32_t sampleRate);
};
