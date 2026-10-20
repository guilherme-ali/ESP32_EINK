#pragma once
#include <stdint.h>

#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
class AudioCodec;

struct ToneTestParams {
  enum class Sequence { Cold, CaptureFirst, Resume };
  bool both = false;
  bool speechLeveling = false;
  float amplitudeDbfs = -24.0f;
  uint32_t frequencyHz = 1000;
  uint32_t durationMs = 1500;
  float volumePercent = 100.0f;
  float dacTrimDb = 0;
  uint32_t settleMs = 0;
  Sequence sequence = Sequence::Cold;
};

// Sincrono, ownership exclusivo do codec; caller deve parar/join todos os workers.
// Fs=24000, PCM16 mono replicado LR, fade-in/out20ms; buffers ~2KiB na stack.
// Bounds: freq100..4000Hz, duracao250..2000ms, amplitude-48..-3dBFS,
// volume0..100%, settle0..1000ms, dacTrimDb0..8dB (default0 mantem ganho anterior).
// NaN/Inf/sequence invalida falham e desligam PA. Fonte e bounds nao mudam com trim.
// Cold: end->prepare; CaptureFirst: capture16k/gain24/read100ms/end->prepare;
// Resume: prepare->enable(false)->enable(true), preservando todos os parametros.
// ToneReady JSON antes de qualquer write PCM, com readback real dos registradores;
// ToneDone JSON apos end (pass/quiescent/framecount de tom aceito, exclui zeros).
// ToneReady keys: mode/amplitude_dbfs/sample_rate_hz/frequency_hz/duration_ms/
// volume_percent/dac_trim_db/settle_ms/sequence/regs ("0x00"/"0x01"/"0x0D"/"0x0E"/
// "0x12"/"0x13"/"0x31"/"0x32"/"0x34", valores numericos lidos do hardware).
// Antes de ToneReady/PCM: pico fonte (DRC <=0.5FS, -6.0205999dBFS) +
// (REG32 real - 191)*0.5 deve ser <=-0.25dBFS; senao ToneDone/dac_headroom.
// Trim diagnostico permitido em DAC/BOTH/DRC; nao muda compensacao hw_gain=6.
// Preserva volume em cache; sem arquivos/NVS/rede. Nao mede potencia/consumo,
// amplitude analogica, SPL nem comprova audio correto ou ganho de producao.
bool runToneTest(AudioCodec &codec, const ToneTestParams &params);
#endif
