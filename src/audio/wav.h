#pragma once
#include <Arduino.h>
#include <FS.h>

// Cabecalho RIFF/WAV canonico de 44 bytes, PCM 16-bit.
#pragma pack(push, 1)
struct WavHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t chunkSize = 0;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1; // PCM
  uint16_t numChannels = 1;
  uint32_t sampleRate = 8000;
  uint32_t byteRate = 0;
  uint16_t blockAlign = 0;
  uint16_t bitsPerSample = 16;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize = 0;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == 44, "WavHeader deve ter 44 bytes");

inline WavHeader makeWavHeader(uint32_t sampleRate, uint16_t channels, uint32_t dataSize) {
  WavHeader h;
  h.sampleRate = sampleRate;
  h.numChannels = channels;
  h.blockAlign = channels * (h.bitsPerSample / 8);
  h.byteRate = sampleRate * h.blockAlign;
  h.dataSize = dataSize;
  h.chunkSize = 36 + dataSize;
  return h;
}

inline bool validWavHeader(const WavHeader &h, size_t actualSize) {
  return actualSize > sizeof(WavHeader) &&
         memcmp(h.riff, "RIFF", 4) == 0 && memcmp(h.wave, "WAVE", 4) == 0 &&
         memcmp(h.fmt, "fmt ", 4) == 0 && memcmp(h.data, "data", 4) == 0 &&
         h.fmtSize == 16 && h.audioFormat == 1 && h.numChannels == 1 &&
         h.bitsPerSample == 16 && h.blockAlign == 2 && h.sampleRate >= 8000 &&
         h.sampleRate <= 48000 && h.byteRate == h.sampleRate * 2 && h.dataSize > 0 &&
         h.dataSize % 2 == 0 && actualSize == sizeof(WavHeader) + h.dataSize &&
         h.chunkSize == 36 + h.dataSize;
}
