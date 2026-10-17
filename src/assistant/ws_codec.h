#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace Assistant {
// RFC 6455, sections 4, 5 and 8. No SDK, allocation or I/O in the parser.
class WsParser {
public:
  enum class Event { None, Message, Ping, Pong, Close, Error, Audio };
  WsParser(uint8_t *storage, size_t capacity);
  Event feed(uint8_t byte);
  bool finish(); // EOF with an incomplete frame/message is an error.
  const uint8_t *data() const { return eventData_; }
  size_t size() const { return eventSize_; }
  const char *error() const { return error_; }
  bool pending() const { return headerUsed_ || fragmented_; }
private:
  Event fail(const char *reason);
  Event complete();
  uint8_t *storage_;
  size_t capacity_, used_ = 0, eventSize_ = 0;
  const uint8_t *eventData_ = nullptr;
  uint8_t header_[10] = {}, control_[125] = {};
  size_t headerUsed_ = 0, headerNeed_ = 2;
  uint64_t length_ = 0, read_ = 0;
  uint8_t opcode_ = 0, messageOpcode_ = 0;
  bool final_ = false, fragmented_ = false, messageReady_ = false;
  const char *error_ = nullptr;
};
size_t wsClientHeader(uint8_t *out, uint8_t opcode, size_t length, const uint8_t mask[4]);
void wsMask(uint8_t *bytes, size_t length, const uint8_t mask[4], size_t offset = 0);
bool wsUpgradeValid(const std::string &headers, const std::string &expectedAccept);
bool validUtf8(const uint8_t *bytes, size_t length);
} // namespace Assistant
