#include "tone_test.h"

#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
#include "codec.h"
#include "speech_leveler.h"
#include <cmath>
#include <cstring>

bool runToneTest(AudioCodec &codec, const ToneTestParams &p) {
  constexpr uint32_t kRate = 24000;
  constexpr uint32_t kChunkFrames = 256;
  constexpr uint32_t kDmaFrames = 4096;
  constexpr uint32_t kFadeFrames = kRate / 50;
  const float previousVolume = codec.volumePercent();
  size_t toneBytes = 0;
  auto finish = [&](bool ok, const char *reason) {
    codec.end();
    const bool quiet = codec.quiescent();
    codec.setVolume(previousVolume); // Cache only, with the rail off.
    Serial.printf("[ToneDone] {\"pass\":%s,\"quiescent\":%s,\"framecount\":%lu,\"reason\":\"%s\"}\n",
                  ok && quiet ? "true" : "false", quiet ? "true" : "false",
                  static_cast<unsigned long>(toneBytes / 4), reason);
    return ok && quiet;
  };
  codec.end();
  if (!codec.quiescent()) return finish(false, "initial_cleanup");
  SpeechLeveler leveler;
  if (p.speechLeveling && (p.both || !leveler.configure(kRate))) return finish(false, "profile");
  const char *sequence = nullptr;
  switch (p.sequence) {
    case ToneTestParams::Sequence::Cold: sequence = "cold"; break;
    case ToneTestParams::Sequence::CaptureFirst: sequence = "capture"; break;
    case ToneTestParams::Sequence::Resume: sequence = "resume"; break;
    default: return finish(false, "sequence");
  }
  if (!std::isfinite(p.amplitudeDbfs) || p.amplitudeDbfs < -48 || p.amplitudeDbfs > -3 ||
      !std::isfinite(p.volumePercent) || p.volumePercent < 0 || p.volumePercent > 100 ||
      !std::isfinite(p.dacTrimDb) || p.dacTrimDb < 0 || p.dacTrimDb > 8 ||
      p.frequencyHz < 100 || p.frequencyHz > 4000 || p.durationMs < 250 ||
      p.durationMs > 2000 || p.settleMs > 1000) return finish(false, "parameters");

  int16_t stereo[kChunkFrames * 2] = {};
  if (p.sequence == ToneTestParams::Sequence::CaptureFirst) {
    if (!codec.prepareCapture(16000, 24)) return finish(false, "capture_prepare");
    size_t remaining = 1600 * 4; // Exactly 100ms of stereo input at 16kHz.
    while (remaining) {
      const size_t wanted = remaining < sizeof(stereo) ? remaining : sizeof(stereo);
      size_t got = 0;
      if (!codec.read(stereo, wanted, &got, 100) || !got || got > wanted || got % 4)
        return finish(false, "capture_read");
      remaining -= got;
    }
    codec.end();
    if (!codec.quiescent()) return finish(false, "capture_cleanup");
  }
  if (!codec.preparePlaybackTest(kRate, p.volumePercent, p.both, p.settleMs, p.dacTrimDb))
    return finish(false, "playback_prepare");
  if (p.sequence == ToneTestParams::Sequence::Resume &&
      (!codec.enable(false) || !codec.enable(true))) return finish(false, "resume");

  constexpr uint8_t addresses[] = {0x00, 0x01, 0x0D, 0x0E, 0x12, 0x13, 0x31, 0x32, 0x34};
  uint16_t regs[sizeof(addresses)] = {};
  for (size_t i = 0; i < sizeof(addresses); ++i) {
    if (!codec.readRegister(addresses[i], regs[i])) return finish(false, "readback");
  }
  // Use the actual half-dB DAC register, including the driver's HW compensation.
  const float sourcePeakDb = p.speechLeveling ? -6.0205999f : p.amplitudeDbfs;
  const float realDacDb = (static_cast<int>(regs[7]) - 191) * 0.5f;
  if (sourcePeakDb + realDacDb > -0.25f) return finish(false, "dac_headroom");
  Serial.printf(
      "[ToneReady] {\"mode\":\"%s\",\"amplitude_dbfs\":%.3f,\"sample_rate_hz\":%lu,\"frequency_hz\":%lu,"
      "\"duration_ms\":%lu,\"volume_percent\":%.3f,\"dac_trim_db\":%.3f,\"settle_ms\":%lu,\"sequence\":\"%s\","
      "\"regs\":{\"0x00\":%u,\"0x01\":%u,\"0x0D\":%u,\"0x0E\":%u,"
      "\"0x12\":%u,\"0x13\":%u,\"0x31\":%u,\"0x32\":%u,\"0x34\":%u}}\n",
       p.speechLeveling ? "drc" : (p.both ? "both" : "dac"), static_cast<double>(p.amplitudeDbfs),
      static_cast<unsigned long>(kRate), static_cast<unsigned long>(p.frequencyHz),
      static_cast<unsigned long>(p.durationMs), static_cast<double>(p.volumePercent),
      static_cast<double>(p.dacTrimDb),
      static_cast<unsigned long>(p.settleMs), sequence,
      static_cast<unsigned>(regs[0]), static_cast<unsigned>(regs[1]),
      static_cast<unsigned>(regs[2]), static_cast<unsigned>(regs[3]),
      static_cast<unsigned>(regs[4]), static_cast<unsigned>(regs[5]),
      static_cast<unsigned>(regs[6]), static_cast<unsigned>(regs[7]),
      static_cast<unsigned>(regs[8]));

  auto writeAll = [&](size_t bytes, bool tone) {
    size_t offset = 0;
    while (offset < bytes) {
      size_t written = 0;
      const bool ok = codec.write(reinterpret_cast<const uint8_t *>(stereo) + offset,
                                  bytes - offset, &written, 250);
      if (written > bytes - offset) return false;
      if (tone) toneBytes += written;
      if (!ok || !written) return false; // Never spin on a stalled/invalid DMA write.
      offset += written;
    }
    return true;
  };
  std::memset(stereo, 0, sizeof(stereo));
  for (uint32_t chunk = 0; chunk < kDmaFrames / kChunkFrames; ++chunk) {
    if (!writeAll(sizeof(stereo), false)) return finish(false, "prefill");
  }
  delay(20); // A short silent lead-in; DMA underflow auto-clears to zero.

  float sine[257];
  constexpr float kTwoPi = 6.2831853071795864769f;
  for (unsigned i = 0; i < 256; ++i) sine[i] = std::sin(kTwoPi * i / 256.0f);
  sine[256] = sine[0];
  const float amplitude = 32767.0f * std::pow(10.0f, p.amplitudeDbfs / 20.0f);
  const uint32_t step = (static_cast<uint64_t>(p.frequencyHz) << 32) / kRate;
  const uint32_t frames = kRate * p.durationMs / 1000;
  uint32_t phase = 0;
  for (uint32_t start = 0; start < frames; start += kChunkFrames) {
    const uint32_t count = frames - start < kChunkFrames ? frames - start : kChunkFrames;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t frame = start + i;
      float envelope = 1.0f;
      if (frame < kFadeFrames) envelope = static_cast<float>(frame) / (kFadeFrames - 1);
      const uint32_t tail = frames - 1 - frame;
      if (tail < kFadeFrames) envelope = static_cast<float>(tail) / (kFadeFrames - 1);
      const unsigned index = phase >> 24;
      const float fraction = static_cast<float>(phase & 0xFFFFFF) / 16777216.0f;
      const float sample = sine[index] + (sine[index + 1] - sine[index]) * fraction;
      const int16_t source = static_cast<int16_t>(std::lround(sample * amplitude * envelope));
      const int16_t pcm = p.speechLeveling ? leveler.process(source) : source;
      stereo[i * 2] = stereo[i * 2 + 1] = pcm;
      phase += step;
    }
    if (!writeAll(count * 4, true)) return finish(false, "tone_write");
  }
  if (p.speechLeveling) {
    int16_t tail;
    size_t count = 0;
    while (leveler.flush(tail)) {
      stereo[count * 2] = stereo[count * 2 + 1] = tail;
      if (++count == kChunkFrames) {
        if (!writeAll(count * 4, false)) return finish(false, "leveler_tail");
        count = 0;
      }
    }
    if (count && !writeAll(count * 4, false)) return finish(false, "leveler_tail");
  }
  // One complete ring pushes every queued tone frame out before end(), as Player does.
  std::memset(stereo, 0, sizeof(stereo));
  for (uint32_t chunk = 0; chunk < kDmaFrames / kChunkFrames; ++chunk) {
    if (!writeAll(sizeof(stereo), false)) return finish(false, "drain");
  }
  return finish(true, "complete");
}
#endif
