#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

// Mono PCM16, single consumer/thread. No heap or platform dependencies.
// Supported: bypass at 8/16/32/44.1/48 kHz; 16->8 and 32/44.1/48->16 kHz.
// configure() starts a new stream; failure leaves the converter disabled.
// reset() starts a new stream while retaining the configured coefficients.
// push() consumes exactly one sample; true means exactly one output was written.
// false leaves output untouched. After the first finish(), push() is prohibited
// (returns false without consuming); call finish() until false, then reset().
//
// Non-bypass is a centered, 193-tap Blackman-windowed sinc, cutoff 0.45*out/in
// cycles/input sample. A 17-phase float bank linearly approximates fractional
// delays; integer ratios use one exact phase. Configuration does the trig work;
// streaming uses float MACs only, with no double, allocation or trig functions.
// The object owns about 14 KiB: keep it off small task stacks.
//
// Group delay is 96 input samples: discard the leading causal delay by starting
// at output time zero, waiting for 97 inputs before returning the first sample.
// finish() zero-pads the right edge and returns all remaining samples, for a
// total ceil(N*out/in), at times k*in/out < N. The finite input is zero outside
// [0,N): boundary samples are filtered (DC edges can ring/attenuate). This is
// delay-compensated, duration-preserving filtering, NOT a lossless transform:
// out-of-band energy and the convolution outside the original duration are not
// returned. Neither a chunk boundary nor EOF silently drops unprocessed input.
class PcmRateConverter {
public:
  bool configure(uint32_t inputHz, uint32_t outputHz) {
    configured_ = false;
    reset();
    if (!supportedRate(inputHz) || !supportedRate(outputHz)) return false;
    if (inputHz != outputHz &&
        !((inputHz == 16000 && outputHz == 8000) ||
          (outputHz == 16000 &&
           (inputHz == 32000 || inputHz == 44100 || inputHz == 48000)))) {
      return false;
    }
    bypass_ = inputHz == outputHz;
    outputHz_ = outputHz;
    step_ = inputHz / outputHz;
    stepRemainder_ = inputHz % outputHz;
    inverseOutputHz_ = 1.0f / static_cast<float>(outputHz);
    if (!bypass_) {
      const float pi = 3.14159265358979323846f;
      const float cutoff = 0.45f * static_cast<float>(outputHz) /
                           static_cast<float>(inputHz);
      for (unsigned phase = 0; phase <= kPhases; ++phase) {
        float sum = 0.0f;
        for (unsigned tap = 0; tap < kTaps; ++tap) {
          const float x = static_cast<float>(tap) - kRadius -
                          static_cast<float>(phase) / kPhases;
          float coefficient = 0.0f;
          if (fabsf(x) < kRadius) {
            const float window = 0.42f + 0.5f * cosf(pi * x / kRadius) +
                                 0.08f * cosf(2.0f * pi * x / kRadius);
            coefficient = (fabsf(x) < 1.0e-6f ? 2.0f * cutoff :
                           sinf(2.0f * pi * cutoff * x) / (pi * x)) * window;
          }
          coefficients_[phase][tap] = coefficient;
          sum += coefficient;
        }
        for (unsigned tap = 0; tap < kTaps; ++tap) {
          coefficients_[phase][tap] /= sum;
        }
      }
    }
    configured_ = true;
    return true;
  }

  bool push(int16_t input, int16_t &output) {
    if (!configured_ || finishing_) return false;
    if (bypass_) {
      output = input;
      return true;
    }
    ++inputCount_;
    append(input);
    if (--waiting_ != 0) return false;
    output = render();
    advance();
    return true;
  }

  bool finish(int16_t &output) {
    finishing_ = true;
    if (!configured_ || bypass_ || nextCenter_ >= inputCount_) return false;
    while (waiting_ != 0) {
      append(0);
      --waiting_;
    }
    output = render();
    advance();
    return true;
  }

  void reset() {
    memset(history_, 0, sizeof(history_));
    writeAt_ = 0;
    waiting_ = kRadius + 1;
    remainder_ = 0;
    inputCount_ = 0;
    nextCenter_ = 0;
    finishing_ = false;
  }

private:
  enum : unsigned { kRadius = 96, kTaps = 193, kPhases = 16, kHistory = 256 };

  static bool supportedRate(uint32_t hz) {
    return hz == 8000 || hz == 16000 || hz == 32000 ||
           hz == 44100 || hz == 48000;
  }

  void append(int16_t input) {
    history_[writeAt_] = input;
    writeAt_ = (writeAt_ + 1) & (kHistory - 1);
  }

  int16_t render() const {
    const uint32_t scaled = remainder_ * kPhases;
    const unsigned phase = scaled / outputHz_;
    const float blend = static_cast<float>(scaled % outputHz_) * inverseOutputHz_;
    const float *a = coefficients_[phase];
    const float *b = coefficients_[phase + 1];
    unsigned at = (writeAt_ + kHistory - kTaps) & (kHistory - 1);
    float sumA = 0.0f;
    float sumB = 0.0f;
    if (remainder_ == 0) {
      for (unsigned tap = 0; tap < kTaps; ++tap) {
        sumA += static_cast<float>(history_[at]) * a[tap];
        at = (at + 1) & (kHistory - 1);
      }
    } else {
      for (unsigned tap = 0; tap < kTaps; ++tap) {
        const float sample = static_cast<float>(history_[at]);
        sumA += sample * a[tap];
        sumB += sample * b[tap];
        at = (at + 1) & (kHistory - 1);
      }
      sumA += blend * (sumB - sumA);
    }
    if (sumA >= 32767.0f) return 32767;
    if (sumA <= -32768.0f) return -32768;
    return static_cast<int16_t>(sumA >= 0.0f ? sumA + 0.5f : sumA - 0.5f);
  }

  void advance() {
    waiting_ = step_;
    remainder_ += stepRemainder_;
    if (remainder_ >= outputHz_) {
      remainder_ -= outputHz_;
      ++waiting_;
    }
    nextCenter_ += waiting_;
  }

  float coefficients_[kPhases + 1][kTaps];
  int16_t history_[kHistory] = {};
  uint64_t inputCount_ = 0;
  uint64_t nextCenter_ = 0;
  uint32_t outputHz_ = 0;
  uint32_t step_ = 0;
  uint32_t stepRemainder_ = 0;
  uint32_t remainder_ = 0;
  unsigned writeAt_ = 0;
  unsigned waiting_ = kRadius + 1;
  float inverseOutputHz_ = 0.0f;
  bool configured_ = false;
  bool bypass_ = false;
  bool finishing_ = false;
};
