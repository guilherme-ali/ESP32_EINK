#include "capture_session.h"
#include "codec.h"
#include "../board/boot_trace.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstdlib>

namespace {
std::atomic<CaptureSession *> captureOwner{nullptr};
constexpr size_t kFrames = 512;
constexpr int64_t kNoSamplesTimeoutUs = 2000000;
}

CaptureSession::~CaptureSession() { end(); }

bool CaptureSession::start(AudioCodec &codec, uint32_t inputRate, float micGainDb) {
  if (active() || !inputRate || inputRate > 48000) return false;
  CaptureSession *expected = nullptr;
  if (!captureOwner.compare_exchange_strong(expected, this)) return false;
  inputRate_ = inputRate < 16000 ? 16000 : inputRate;
  failed_ = false; overflowCount_ = 0; stopRequested_ = false;
  closing_ = false; readStarted_ = false; drained_ = false;
  pendingCount_ = pendingOffset_ = 0;
  active_ = true;
  const size_t bytes = static_cast<size_t>(inputRate_) * sizeof(int16_t) * 8;
  ringStorage_ = static_cast<uint8_t *>(heap_caps_malloc(bytes + 1, MALLOC_CAP_SPIRAM));
  ringStruct_ = malloc(sizeof(StaticStreamBuffer_t));
  if (ringStorage_ && ringStruct_)
    ring_ = xStreamBufferCreateStatic(bytes + 1, 1, ringStorage_, static_cast<StaticStreamBuffer_t *>(ringStruct_));
  if (!ring_ || !converter_.configure(inputRate_, inputRate_)) {
    failed_ = true; end(); return false;
  }
  codec_ = &codec;
  if (!codec.prepareCapture(inputRate_, micGainDb)) {
    failed_ = true; end(); return false;
  }
  BootTrace::mark(BootTrace::AudioReady);
  producerDone_ = false;
  if (xTaskCreatePinnedToCore(producerFn, "session_cap", 4096, this, 5, &producer_, 0) != pdPASS) {
    producerDone_ = true; failed_ = true; end(); return false;
  }
  return true;
}

bool CaptureSession::setOutputRate(uint32_t outputRate) {
  if (!active() || readStarted_ || !outputRate || outputRate > inputRate_) return false;
  return converter_.configure(inputRate_, outputRate);
}

void CaptureSession::producerFn(void *arg) {
  auto *self = static_cast<CaptureSession *>(arg);
  int16_t stereo[kFrames * 2];
  auto ring = static_cast<StreamBufferHandle_t>(self->ring_);
  int64_t lastSamplesUs = esp_timer_get_time();
  while (!self->stopRequested_.load()) {
    size_t bytes = 0;
    if (!self->codec_->read(stereo, sizeof(stereo), &bytes, 100) || bytes > sizeof(stereo) || bytes % 4) {
      self->failed_ = true; self->requestStop(); break;
    }
    if (!bytes) {
      if (self->stopRequested_.load()) break;
      if (esp_timer_get_time() - lastSamplesUs >= kNoSamplesTimeoutUs) {
        self->failed_ = true; self->requestStop(); break;
      }
      vTaskDelay(1);
      continue;
    }
    lastSamplesUs = esp_timer_get_time();
    BootTrace::mark(BootTrace::FirstBlock);
    size_t frames = bytes / 4;
    for (size_t i = 0; i < frames; ++i) stereo[i] = stereo[i * 2];
    const size_t monoBytes = frames * sizeof(int16_t);
    // Nunca enviar parcialmente: a fila cheia preserva somente o prefixo.
    if (xStreamBufferSpacesAvailable(ring) < monoBytes ||
        xStreamBufferSend(ring, stereo, monoBytes, 0) != monoBytes) {
      ++self->overflowCount_; self->failed_ = true; self->requestStop(); break;
    }
  }
  // Ultimo acesso a self: end() pode liberar objeto/ring/codec apos este store.
  // vTaskDelete(nullptr) usa apenas a task corrente; stack/TCB sao do FreeRTOS.
  self->producerDone_ = true;
  vTaskDelete(nullptr);
}

bool CaptureSession::read(int16_t *mono, size_t capacity, size_t *samplesRead, uint32_t timeoutMs) {
  if (!samplesRead) return false;
  *samplesRead = 0;
  if (!mono || !capacity || !active() || closing_.load()) return false;
  bool expected = false;
  if (!reading_.compare_exchange_strong(expected, true)) return false;
  if (closing_.load()) { reading_ = false; return false; }
  readStarted_ = true;
  auto ring = static_cast<StreamBufferHandle_t>(ring_);
  size_t count = 0;
  TickType_t wait = pdMS_TO_TICKS(timeoutMs);
  while (count < capacity) {
    if (pendingOffset_ == pendingCount_) {
      pendingOffset_ = 0;
      pendingCount_ = xStreamBufferReceive(ring, pending_, sizeof(pending_), wait) / sizeof(int16_t);
      wait = 0; // Nunca multiplicar o timeout pelo numero de blocos/amostras.
      if (!pendingCount_) {
        // Reavaliar fila apos observar o estado terminal do produtor.
        if (!finished() || !xStreamBufferIsEmpty(ring)) break;
        if (converter_.finish(mono[count])) { ++count; continue; }
        drained_ = true;
        break;
      }
    }
    if (converter_.push(pending_[pendingOffset_++], mono[count])) ++count;
  }
  *samplesRead = count;
  const bool ok = count || !failed();
  reading_ = false;
  return ok;
}

void CaptureSession::end() {
  if (!active()) return;
  closing_ = true;
  requestStop();
  while (!producerDone_.load() || reading_.load()) vTaskDelay(1);
  // O produtor jamais desliga o codec: ainda pode haver audio para consumir.
  if (codec_) codec_->end();
  codec_ = nullptr;
  if (ring_) vStreamBufferDelete(static_cast<StreamBufferHandle_t>(ring_));
  ring_ = nullptr;
  free(ringStorage_); free(ringStruct_);
  ringStorage_ = nullptr; ringStruct_ = nullptr; producer_ = nullptr;
  pendingCount_ = pendingOffset_ = 0;
  converter_.reset(); drained_ = true; active_ = false;
  CaptureSession *expected = this;
  captureOwner.compare_exchange_strong(expected, nullptr);
}
