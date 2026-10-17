#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ESP32)

struct OpusEncoder;
struct OpusDecoder;

// Raw Opus packets, not Ogg. One owner/task at a time; no internal locking.
// The protocol owns packet lengths, final-frame zero padding, flushing the
// encoder lookahead and trimming pre-skip/end padding. No implicit PLC or FEC.
class VoiceOpus {
public:
  static constexpr size_t kEncodeSamples = 320; // Mono PCM16, 16 kHz, 20 ms.
  static constexpr size_t kDecodeSamples = 480; // Mono PCM16, 24 kHz, 20 ms.
  static constexpr size_t kMaxPacketBytes = 1276; // TOC + one Opus frame.

  VoiceOpus() = default;
  ~VoiceOpus();
  VoiceOpus(const VoiceOpus &) = delete;
  VoiceOpus &operator=(const VoiceOpus &) = delete;

  // Allocates states once (PSRAM preferred, internal RAM fallback). Repeating
  // begin reinitializes existing storage; reset/encode/decode never allocate.
  // libopus uses C99 stack scratch: provide a sufficiently large task stack.
  bool begin(int bitrate = 64000);
  // Returns packet bytes, or a negative libopus error. Exactly 320 samples.
  int encode(const int16_t *pcm, size_t samples, uint8_t *out, size_t cap);
  // Returns 480 samples, or a negative libopus error. Only mono 20 ms packets.
  int decode(const uint8_t *packet, size_t n, int16_t *out, size_t capSamples);
  // Delay in input (16 kHz) samples, NOT in 24 kHz output samples or Ogg 48 kHz
  // units. Returns zero before begin. Does not include final-frame padding.
  int encoderLookahead() const;
  void reset();
  // Segment boundaries in one direction must not reset the other stream.
  void resetEncoder();
  void resetDecoder();
  const char *error() const;

private:
  OpusEncoder *encoder_ = nullptr;
  OpusDecoder *decoder_ = nullptr;
  int lookahead_ = 0;
  int lastError_ = -6; // OPUS_INVALID_STATE, until begin succeeds.
  bool ready_ = false;

  int result(int code);
  void release();
};

#endif
