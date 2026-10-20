#pragma once
#include <Arduino.h>
#include <freertos/task.h>
#include <atomic>
#include "pcm_rate_converter.h"

class AudioCodec;

// Unico produtor I2S e unico consumidor. start/setOutputRate/end pertencem
// ao dono; end pode cancelar read, mas o consumidor deve parar antes do reuso.
class CaptureSession {
public:
  CaptureSession() = default;
  ~CaptureSession();
  CaptureSession(const CaptureSession &) = delete;
  CaptureSession &operator=(const CaptureSession &) = delete;

  // I2S aceita blocos menores alinhados a frames LR PCM16. Erro, tamanho
  // invalido ou 2s continuos sem amostras encerram produtor com failed=true.
  bool start(AudioCodec &codec, uint32_t inputRate, float micGainDb);
  bool setOutputRate(uint32_t outputRate);
  // Timeout/EOF saudavel retorna true com zero amostras. Falha retorna false
  // quando nao ha mais prefixo para entregar; sempre conferir samplesRead.
  bool read(int16_t *mono, size_t capacity, size_t *samplesRead, uint32_t timeoutMs = 100);
  void requestStop() { stopRequested_.store(true); }
  bool finished() const { return producerDone_.load(); }
  bool drained() const { return drained_.load(); }
  bool failed() const { return failed_.load(); }
  uint32_t overflowCount() const { return overflowCount_.load(); }
  uint32_t inputRate() const { return inputRate_; }
  bool active() const { return active_.load(); }
  void end();

private:
  AudioCodec *codec_ = nullptr;
  uint32_t inputRate_ = 0;
  void *ring_ = nullptr;
  void *ringStruct_ = nullptr;
  uint8_t *ringStorage_ = nullptr;
  TaskHandle_t producer_ = nullptr;
  PcmRateConverter converter_{};
  int16_t pending_[256]{};
  size_t pendingCount_ = 0, pendingOffset_ = 0;
  bool readStarted_ = false;
  std::atomic<bool> active_{false}, stopRequested_{false}, producerDone_{true};
  std::atomic<bool> failed_{false}, drained_{true}, closing_{false}, reading_{false};
  std::atomic<uint32_t> overflowCount_{0};
  static void producerFn(void *arg);
};
