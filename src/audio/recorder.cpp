#include "recorder.h"
#include "wav.h"
#include <LittleFS.h>

namespace {
constexpr int kChunkFrames = 256; // frames estereo por leitura do I2S
int16_t stereoBuf[kChunkFrames * 2];
int16_t monoBuf[kChunkFrames];
} // namespace

bool Recorder::start(const char *path, uint32_t sampleRate) {
  file_ = LittleFS.open(path, FILE_WRITE);
  if (!file_) return false;

  sampleRate_ = sampleRate;
  dataBytes_ = 0;

  WavHeader placeholder = makeWavHeader(sampleRate_, 1, 0);
  file_.write((const uint8_t *)&placeholder, sizeof(placeholder));

  active_ = true;
  return true;
}

bool Recorder::feed(AudioCodec &codec) {
  if (!active_) return false;

  size_t bytesRead = 0;
  if (!codec.read(stereoBuf, sizeof(stereoBuf), &bytesRead) || bytesRead == 0) {
    return false;
  }

  int frames = bytesRead / (2 * sizeof(int16_t));
  for (int i = 0; i < frames; i++) {
    monoBuf[i] = stereoBuf[i * 2]; // canal esquerdo
  }

  size_t monoBytes = frames * sizeof(int16_t);
  size_t written = file_.write((const uint8_t *)monoBuf, monoBytes);
  dataBytes_ += written;
  return written == monoBytes;
}

uint32_t Recorder::stop() {
  if (!active_) return dataBytes_;
  active_ = false;

  WavHeader hdr = makeWavHeader(sampleRate_, 1, dataBytes_);
  file_.seek(0);
  file_.write((const uint8_t *)&hdr, sizeof(hdr));
  file_.close();
  return dataBytes_;
}
