#include "voice_opus.h"

#if defined(ESP32)

#include <esp_heap_caps.h>
#include <opus.h>
#include <limits.h>

namespace {
void *allocateState(size_t bytes) {
  void *state = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return state ? state : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
} // namespace

VoiceOpus::~VoiceOpus() {
  release();
}

void VoiceOpus::release() {
  heap_caps_free(encoder_);
  heap_caps_free(decoder_);
  encoder_ = nullptr;
  decoder_ = nullptr;
  ready_ = false;
  lookahead_ = 0;
}

int VoiceOpus::result(int code) {
  lastError_ = code < 0 ? code : OPUS_OK;
  return code;
}

bool VoiceOpus::begin(int bitrate) {
  if (bitrate < 500 || bitrate > 512000) {
    result(OPUS_BAD_ARG);
    return false;
  }
  ready_ = false;
  lookahead_ = 0;
  const int encoderBytes = opus_encoder_get_size(1);
  const int decoderBytes = opus_decoder_get_size(1);
  if (encoderBytes <= 0 || decoderBytes <= 0) {
    release();
    result(OPUS_INTERNAL_ERROR);
    return false;
  }
  if (!encoder_) encoder_ = static_cast<OpusEncoder *>(allocateState(encoderBytes));
  if (!decoder_) decoder_ = static_cast<OpusDecoder *>(allocateState(decoderBytes));
  if (!encoder_ || !decoder_) {
    release();
    result(OPUS_ALLOC_FAIL);
    return false;
  }

  int code = opus_encoder_init(encoder_, 16000, 1, OPUS_APPLICATION_AUDIO);
  if (code == OPUS_OK) code = opus_decoder_init(decoder_, 24000, 1);
  if (code == OPUS_OK) code = opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
  if (code == OPUS_OK) code = opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
  if (code == OPUS_OK) code = opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(1));
  if (code == OPUS_OK) code = opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead_));
  if (code != OPUS_OK) {
    release();
    result(code);
    return false;
  }
  ready_ = true;
  result(OPUS_OK);
  return true;
}

int VoiceOpus::encode(const int16_t *pcm, size_t samples, uint8_t *out, size_t cap) {
  if (!ready_) return result(OPUS_INVALID_STATE);
  if (!pcm || !out || samples != kEncodeSamples) return result(OPUS_BAD_ARG);
  if (!cap) return result(OPUS_BUFFER_TOO_SMALL);
  const size_t packetCap = cap < kMaxPacketBytes ? cap : kMaxPacketBytes;
  return result(opus_encode(encoder_, pcm, kEncodeSamples, out,
                            static_cast<opus_int32>(packetCap)));
}

int VoiceOpus::decode(const uint8_t *packet, size_t n, int16_t *out, size_t capSamples) {
  if (!ready_) return result(OPUS_INVALID_STATE);
  if (!packet || !n || !out || n > INT32_MAX) return result(OPUS_BAD_ARG);
  if (capSamples < kDecodeSamples) return result(OPUS_BUFFER_TOO_SMALL);
  const auto bytes = static_cast<opus_int32>(n);
  const int samples = opus_packet_get_nb_samples(packet, bytes, 24000);
  if (samples < 0) return result(samples);
  if (samples != static_cast<int>(kDecodeSamples) ||
      opus_packet_get_nb_channels(packet) != 1) return result(OPUS_INVALID_PACKET);
  return result(opus_decode(decoder_, packet, bytes, out, kDecodeSamples, 0));
}

int VoiceOpus::encoderLookahead() const {
  return lookahead_;
}

void VoiceOpus::reset() {
  resetEncoder();
  const int encoderCode = lastError_;
  resetDecoder();
  if (encoderCode != OPUS_OK) result(encoderCode);
}

void VoiceOpus::resetEncoder() {
  result(ready_ ? opus_encoder_ctl(encoder_, OPUS_RESET_STATE) : OPUS_INVALID_STATE);
}

void VoiceOpus::resetDecoder() {
  result(ready_ ? opus_decoder_ctl(decoder_, OPUS_RESET_STATE) : OPUS_INVALID_STATE);
}

const char *VoiceOpus::error() const {
  return opus_strerror(lastError_);
}

#endif
