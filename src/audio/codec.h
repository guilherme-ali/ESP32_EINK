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
// Inicia Wire se necessario; nunca encerra o barramento compartilhado com RTC.
// O caller deve parar/join os consumidores antes de trocar modo ou chamar end.
class AudioCodec {
public:
  bool begin();
  bool prepareCapture(uint32_t sampleRate, float micGainDb);
  // boundedSpeech exige PCM vindo do SpeechLeveler, pico <=0.5FS; nao e auto-gain generico.
  // Opt-in: boost DAC 0 ate60%, rampa linear ate+4dB em100%. Default mantem ganho nominal.
  bool preparePlayback(uint32_t sampleRate, float percent, bool boundedSpeech = false);
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  // Diagnostico A/B: ambos mantem Playback/TX-only; so codec_mode DAC/BOTH muda.
  // settleMs (0..1000) apos enable do ES8311, ainda muted/PA off; preservado no resume.
  // trimDb (finito, 0..8dB) soma ganho DAC apos DSP; preservado no resume/rate.
  // Trim absoluto: ignora a curva boundedSpeech, inclusive apos setVolume/resume.
  // Caller exclusivo, sem consumidores ativos. Producao permanece DAC/settle=0.
  bool preparePlaybackTest(uint32_t sampleRate, float percent, bool both, uint32_t settleMs,
                           float trimDb = 0);
  // Le o hardware via get_reg; em erro nao modifica value. Nao e um cache.
  bool readRegister(uint8_t address, uint16_t &value);
#endif
  void end();
  bool quiescent() const { return !i2sInited_ && !codecOpen_ && codec_ == nullptr; }

  // sampleRate deve ter uma entrada na tabela de coeficientes do ES8311
  // (8000, 16000, 32000, 44100, 48000, ...) com mclk = sampleRate*256.
  bool setSampleRate(uint32_t sampleRate);

  bool enable(bool on);
  // UI percent: 0 mutes; 100 is nominal unless boundedSpeech/diagnostic opt-in.
  bool setVolume(float percent);
  float volumePercent() const { return volumePercent_; }
  bool setMicGain(float db);

  // len em bytes; buffers sao PCM 16-bit estereo intercalado (LRLR...).
  bool read(void *buf, size_t len, size_t *bytesRead, uint32_t timeoutMs = 1000);
  bool write(const void *buf, size_t len, size_t *bytesWritten, uint32_t timeoutMs = 1000);

private:
  enum class Mode { Legacy, Capture, Playback };
  static constexpr i2s_port_t kPort = I2S_NUM_0;

  bool i2sInited_ = false;
  bool codecOpen_ = false;
  Mode mode_ = Mode::Legacy;
  uint32_t sampleRate_ = 16000;
  float micGainDb_ = 0;
  float volumePercent_ = 0;
  bool boundedSpeech_ = false;
  const void *codec_ = nullptr; // audio_codec_if_t* (tipo C, evita expor o header aqui)
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  bool playbackBoth_ = false;
  uint32_t playbackSettleMs_ = 0;
  float playbackTrimDb_ = 0;
#endif

  bool open(Mode mode);
  bool startPlayback(uint32_t sampleRate, float percent);
  bool stopI2s();
  bool initI2s(uint32_t sampleRate);
  bool applyVolume();
};
