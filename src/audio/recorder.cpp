#include "recorder.h"
#include "wav.h"
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <esp_heap_caps.h>

namespace {
constexpr size_t kRingBytes = 512 * 1024; // ~8s de audio estereo a 16kHz
constexpr size_t kChunkBytes = 2048;      // 512 frames estereo por vez (= dma_buf_len do codec)
constexpr UBaseType_t kCapturePriority = 5;
constexpr UBaseType_t kWritePriority = 3;
constexpr BaseType_t kAudioCore = 0; // loop() do Arduino roda no core 1
} // namespace

bool Recorder::start(const char *path, uint32_t sampleRate, AudioCodec *codec) {
  if (active_) return false;

  file_ = LittleFS.open(path, FILE_WRITE);
  if (!file_) return false;

  sampleRate_ = sampleRate;
  codec_ = codec;
  dataBytes_ = 0;
  overflowCount_ = 0;
  stopRequested_ = false;

  WavHeader placeholder = makeWavHeader(sampleRate_, 1, 0);
  file_.write((const uint8_t *)&placeholder, sizeof(placeholder));

  if (!ringStorage_) {
    ringStorage_ = (uint8_t *)heap_caps_malloc(kRingBytes + 1, MALLOC_CAP_SPIRAM);
    ringStruct_ = malloc(sizeof(StaticStreamBuffer_t));
    if (!ringStorage_ || !ringStruct_) return false;
    ring_ = xStreamBufferCreateStatic(kRingBytes, 1, ringStorage_,
                                       (StaticStreamBuffer_t *)ringStruct_);
    if (!ring_) return false;
  } else {
    xStreamBufferReset((StreamBufferHandle_t)ring_);
  }

  captureTaskDone_ = false;
  writeTaskDone_ = false;
  active_ = true;

  xTaskCreatePinnedToCore(captureTaskFn, "audio_cap", 4096, this, kCapturePriority,
                           &captureTaskHandle_, kAudioCore);
  // File::write() do LittleFS estourou uma stack de 4096 bytes na
  // pratica (Guru Meditation: "Stack canary watchpoint triggered
  // (audio_wr)") - o driver de flash usa varios KB internamente.
  xTaskCreatePinnedToCore(writeTaskFn, "audio_wr", 8192, this, kWritePriority,
                           &writeTaskHandle_, kAudioCore);
  return true;
}

void Recorder::captureTaskFn(void *arg) {
  Recorder *self = (Recorder *)arg;
  uint8_t buf[kChunkBytes];
  auto ring = (StreamBufferHandle_t)self->ring_;

  while (!self->stopRequested_) {
    size_t bytesRead = 0;
    if (self->codec_->read(buf, sizeof(buf), &bytesRead, 200) && bytesRead > 0) {
      size_t sent = xStreamBufferSend(ring, buf, bytesRead, pdMS_TO_TICKS(50));
      if (sent < bytesRead) {
        self->overflowCount_++;
      }
    }
  }

  self->captureTaskDone_ = true;
  vTaskDelete(nullptr);
}

void Recorder::writeTaskFn(void *arg) {
  Recorder *self = (Recorder *)arg;
  auto ring = (StreamBufferHandle_t)self->ring_;

  // static, nao na stack da task - File::write() do LittleFS ja usa
  // varios KB de stack sozinho (CRC/wear-leveling internos), sobra
  // pouco espaco para buffers locais grandes.
  static uint8_t stereoBuf[kChunkBytes];
  static int16_t monoBuf[kChunkBytes / 4];

  for (;;) {
    size_t got = xStreamBufferReceive(ring, stereoBuf, sizeof(stereoBuf), pdMS_TO_TICKS(100));
    if (got > 0) {
      int frames = got / (2 * sizeof(int16_t));
      const int16_t *stereoSamples = (const int16_t *)stereoBuf;
      for (int i = 0; i < frames; i++) {
        monoBuf[i] = stereoSamples[i * 2]; // canal esquerdo
      }
      size_t monoBytes = frames * sizeof(int16_t);
      size_t written = self->file_.write((const uint8_t *)monoBuf, monoBytes);
      self->dataBytes_ += written;
    }

    if (self->stopRequested_ && xStreamBufferIsEmpty(ring)) break;
  }

  self->writeTaskDone_ = true;
  vTaskDelete(nullptr);
}

uint32_t Recorder::stop() {
  if (!active_) return dataBytes_;

  stopRequested_ = true;
  while (!captureTaskDone_ || !writeTaskDone_) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  active_ = false;

  WavHeader hdr = makeWavHeader(sampleRate_, 1, dataBytes_);
  file_.seek(0);
  file_.write((const uint8_t *)&hdr, sizeof(hdr));
  file_.close();
  return dataBytes_;
}
