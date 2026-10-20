#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>

// Digital source telemetry only, before the DAC/volume/PA. Not acoustic SPL.
struct PlaybackLevel {
  uint64_t samples = 0;
  uint64_t squares = 0;
  uint32_t peak = 0;
  uint32_t fullScaleSamples = 0;
  uint32_t windowSamples = 2400;
  uint32_t windowCount = 0;
  uint64_t windowSquares = 0, maxWindowSquares = 0;
  uint32_t fullWindows = 0;

  void add(const int16_t *pcm, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      const int32_t value = pcm[i];
      const uint32_t magnitude = value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
      if (magnitude > peak) peak = magnitude;
      if (magnitude >= 32767) ++fullScaleSamples;
      squares += static_cast<uint64_t>(static_cast<int64_t>(value) * value);
      windowSquares += static_cast<uint64_t>(static_cast<int64_t>(value) * value);
      if (++windowCount >= windowSamples) {
        if (windowSquares > maxWindowSquares) maxWindowSquares = windowSquares;
        ++fullWindows;
        windowCount = 0;
        windowSquares = 0;
      }
    }
    samples += count;
  }
  double rmsDbfs() const {
    return samples && squares ? 10.0 * std::log10(static_cast<double>(squares) / samples / 1073741824.0) : -160.0;
  }
  double peakDbfs() const { return peak ? 20.0 * std::log10(peak / 32768.0) : -160.0; }
  double maxWindowDbfs() const {
    if (!fullWindows) return rmsDbfs();
    return maxWindowSquares ? 10.0 * std::log10(static_cast<double>(maxWindowSquares) / windowSamples / 1073741824.0) : -160.0;
  }
};
