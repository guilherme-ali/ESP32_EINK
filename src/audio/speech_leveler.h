#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Mono playback only. Single owner, fixed storage, no allocation or SDK calls.
class SpeechLeveler {
public:
  bool configure(uint32_t rate) {
    latency_ = 0;
    if (rate != 8000 && rate != 11025 && rate != 12000 && rate != 16000 &&
        rate != 22050 && rate != 24000 && rate != 32000 && rate != 44100 &&
        rate != 48000 && rate != 96000) {
      reset();
      return false;
    }
    latency_ = (rate + 199) / 200; // ceil(5 ms), <= 480 of the 512 PCM slots.
    shortLookahead_ = static_cast<uint16_t>((rate + 799) / 800); // >= 1.25 ms, <= 120.
    crestHoldSamples_ = (rate + 19) / 20;
    rmsStep_ = 1.0f - std::exp(-1.0f / (0.020f * rate));
    attackStep_ = 1.0f - std::exp(-1.0f / (0.005f * rate));
    releaseStep_ = 1.0f - std::exp(-1.0f / (0.120f * rate));
    peakAttackStep_ = 1.0f - std::exp(-1.0f / (0.0001f * rate));
    peakReleaseStep_ = 1.0f - std::exp(-1.0f / (0.020f * rate));
    crestReleaseStep_ = 1.0f - std::exp(-1.0f / (0.001f * rate));
    reset();
    return true;
  }

  void reset() {
    std::memset(delay_, 0, sizeof(delay_));
    cursor_ = remaining_ = controlPhase_ = 0;
    peakHead_ = peakTail_ = peakCount_ = 0;
    shortHead_ = shortTail_ = shortCount_ = 0;
    crestHold_ = 0;
    rmsPower_ = 0;
    gain_ = targetGain_ = 1;
    peakGain_ = maxGain();
    gateOpen_ = flushing_ = false;
    hardGuards_ = 0;
    peak_ = 0;
  }

  // The first latencySamples() output samples are zero, including short input.
  // process after EOF/invalid configure returns zero; reset/configure starts anew.
  int16_t process(int16_t sample) {
    if (!latency_ || flushing_) return 0;
    return step(sample);
  }

  // EOF: emits exactly L remaining samples, then false forever (also for N=0).
  // Total process+flush output is N+L, not N+2L.
  bool flush(int16_t &out) {
    if (!flushing_) {
      flushing_ = true;
      remaining_ = latency_;
    }
    if (!remaining_) return false;
    --remaining_;
    out = step(0);
    return true;
  }

  size_t latencySamples() const { return latency_; }
  uint64_t hardGuardSamples() const { return hardGuards_; }
  uint32_t peakMagnitude() const { return peak_; }
  static int32_t ceilingPcm() { return 16384; } // <= -6 dBFS and <= 0.5 FS.

private:
  static float maxGain() { return 1.995262315f; } // +6 dB, not +6.02 dB.
  static uint32_t magnitudeOf(int16_t sample) {
    const int32_t wide = sample; // INT16_MIN must not be negated in int16_t.
    return static_cast<uint32_t>(wide < 0 ? -wide : wide);
  }
  int16_t delay_[512] = {};
  uint16_t peakIndices_[512] = {};
  uint16_t shortIndices_[128] = {};
  size_t latency_ = 0, cursor_ = 0, remaining_ = 0;
  uint16_t peakHead_ = 0, peakTail_ = 0, peakCount_ = 0;
  uint16_t shortHead_ = 0, shortTail_ = 0, shortCount_ = 0, shortLookahead_ = 0;
  uint32_t controlPhase_ = 0, crestHoldSamples_ = 0, crestHold_ = 0;
  float rmsStep_ = 0, attackStep_ = 0, releaseStep_ = 0;
  float peakAttackStep_ = 0, peakReleaseStep_ = 0, crestReleaseStep_ = 0;
  float rmsPower_ = 0, gain_ = 1, targetGain_ = 1;
  float peakGain_ = maxGain();
  bool gateOpen_ = false, flushing_ = false;
  uint64_t hardGuards_ = 0;
  uint32_t peak_ = 0;

  int16_t step(int16_t sample) {
    const int32_t wide = sample;
    const uint32_t magnitude = magnitudeOf(sample);
    const float normalized = wide * (1.0f / 32768.0f);
    rmsPower_ += rmsStep_ * (normalized * normalized - rmsPower_);
    if (normalized * normalized > 6.25f * rmsPower_) crestHold_ = crestHoldSamples_;
    else if (crestHold_) --crestHold_;
    if (++controlPhase_ == 16) {
      controlPhase_ = 0;
      gateOpen_ = rmsPower_ >= 0.00001f; // -50 dBFS: never boost background noise.
      targetGain_ = 1;
      if (gateOpen_) {
        const float over = 10.0f * std::log10(rmsPower_) + 18.0f;
        float reduction = 0;
        if (over >= 3) reduction = (2.0f / 3.0f) * over;
        else if (over > -3) reduction = (over + 3) * (over + 3) / 18.0f;
        targetGain_ = std::pow(10.0f, (6.0f - reduction) / 20.0f);
        if (targetGain_ > maxGain()) targetGain_ = maxGain();
      }
    }
    gain_ += (targetGain_ < gain_ ? attackStep_ : releaseStep_) * (targetGain_ - gain_);

    const int16_t delayed = delay_[cursor_];
    const uint32_t delayedMagnitude = magnitudeOf(delayed);
    // Short detector follows delayed output, not undelayed input. Its future
    // slot is still valid here; consume/expire every reference before overwrite.
    size_t future = cursor_ + shortLookahead_;
    if (future >= latency_) future -= latency_;
    const uint32_t futureMagnitude = magnitudeOf(delay_[future]);
    while (shortCount_) {
      const uint16_t back = (shortTail_ + 127) & 127;
      if (magnitudeOf(delay_[shortIndices_[back]]) > futureMagnitude) break;
      shortTail_ = back;
      --shortCount_;
    }
    shortIndices_[shortTail_] = static_cast<uint16_t>(future);
    shortTail_ = (shortTail_ + 1) & 127;
    ++shortCount_; // <= A+1 <= 121, including delayed sample until expiration.
    uint32_t shortMaximum = magnitudeOf(delay_[shortIndices_[shortHead_]]);
    if (delayedMagnitude > shortMaximum) shortMaximum = delayedMagnitude;
    if (shortIndices_[shortHead_] == cursor_) {
      shortHead_ = (shortHead_ + 1) & 127;
      --shortCount_;
    }
    // Monotone deque of raw ring indices, amortized O(1): each index is pushed
    // and removed once. Expire BEFORE reusing its raw slot (no timestamp/alias).
    if (peakCount_ && peakIndices_[peakHead_] == cursor_) {
      peakHead_ = (peakHead_ + 1) & 511;
      --peakCount_;
    }
    delay_[cursor_] = sample;
    while (peakCount_) {
      const uint16_t back = (peakTail_ + 511) & 511;
      if (magnitudeOf(delay_[peakIndices_[back]]) > magnitude) break;
      peakTail_ = back;
      --peakCount_;
    }
    peakIndices_[peakTail_] = static_cast<uint16_t>(cursor_);
    peakTail_ = (peakTail_ + 1) & 511;
    ++peakCount_; // <= L <= 480, including during startup/flush.
    // Deque covers the next L samples including undelayed input; include the
    // fetched delayed sample separately for the exact L+1-sample maximum.
    uint32_t maximum = magnitudeOf(delay_[peakIndices_[peakHead_]]);
    if (delayedMagnitude > maximum) maximum = delayedMagnitude;
    if (crestHold_) maximum = shortMaximum;
    float cap = maximum ? (ceilingPcm() - 1.0f) / maximum : maxGain();
    if (cap > maxGain()) cap = maxGain();
    const float release = crestHold_ ? crestReleaseStep_ : peakReleaseStep_;
    peakGain_ += (cap < peakGain_ ? peakAttackStep_ : release) * (cap - peakGain_);
    if (++cursor_ == latency_) cursor_ = 0;
    float applied = gateOpen_ || gain_ < 1 ? gain_ : 1;
    if (peakGain_ < applied) applied = peakGain_;
    float value = delayed * applied;
    // Both detectors cover each sample for at least A steps, even when switching
    // modes. A >= 12.5 attack time constants: residual <= 1.5*exp(-12.5)*32768
    // < 0.19 PCM. The 1-PCM margin also covers float rounding. Numeric safety,
    // not the normal limiter; tests require zero guards even on abrupt peaks.
    if (!std::isfinite(value)) { value = 0; ++hardGuards_; }
    else if (value > ceilingPcm()) { value = static_cast<float>(ceilingPcm()); ++hardGuards_; }
    else if (value < -ceilingPcm()) { value = static_cast<float>(-ceilingPcm()); ++hardGuards_; }
    const int16_t output = static_cast<int16_t>(value); // Truncate toward zero.
    const int32_t outputWide = output;
    const uint32_t outputMagnitude = static_cast<uint32_t>(outputWide < 0 ? -outputWide : outputWide);
    if (outputMagnitude > peak_) peak_ = outputMagnitude;
    return output;
  }
};
