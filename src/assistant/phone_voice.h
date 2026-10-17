#pragma once
#include "media_protocol.h"
#include "ws_codec.h"
#include <cstdint>
#include <cstddef>
#include <string>

#if defined(ARDUINO_ARCH_ESP32)
#include "../audio/voice_opus.h"
namespace Assistant {
class PhoneVoice {
public:
  using Stop = bool (*)(void *);
  PhoneVoice(uint8_t *storage, size_t capacity, Stop stop, void *context);
  ~PhoneVoice();
  bool connect(const char *key, int bitrate); // bitrate zero selects lossless PCM.
  bool sendJson(const std::string &json);
  bool sendAudio(const uint8_t *pcm, size_t size);
  bool endInput();
  WsParser::Event poll();
  const uint8_t *data() const { return audioResult_ ? reinterpret_cast<const uint8_t *>(decoded_) : storage_; }
  size_t size() const { return resultSize_; }
  bool pending();
  void close();
  const char *error() const { return error_; }
  uint32_t sentBytes() const { return sentBytes_; }
  uint32_t receivedBytes() const { return receivedBytes_; }
  uint32_t encodeUs() const { return encodeUs_; }
  uint32_t decodeUs() const { return decodeUs_; }
private:
  bool record(Media::Type type, const uint8_t *p, size_t n, uint32_t position = 0, uint8_t flags = 0);
  bool encodeFrame();
  bool fail(const char *error);
  VoiceOpus opus_;
  uint8_t *storage_;
  size_t capacity_, headerAt_ = 0, payloadAt_ = 0, resultSize_ = 0, staged_ = 0;
  Stop stop_;
  void *context_;
  int channel_ = -1, bitrate_ = 0;
  uint8_t wire_[Media::kHeaderSize]{};
  int16_t input_[320]{}, decoded_[480]{};
  Media::Header header_;
  uint32_t original_ = 0, encoded_ = 0, emitted_ = 0, receivedSamples_ = 0;
  uint32_t sentBytes_ = 0, receivedBytes_ = 0, pendingSince_ = 0;
  uint32_t encodeUs_ = 0, decodeUs_ = 0;
  uint16_t outputLookahead_ = 0;
  bool ended_ = false, audioResult_ = false, hello_ = false, first_ = true, outputResetAllowed_ = true;
  const char *error_ = "Falha no canal de midia BLE.";
};
}
#endif
