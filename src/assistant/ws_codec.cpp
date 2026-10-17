#include "ws_codec.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace Assistant {
bool validUtf8(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n;) {
    uint32_t c = p[i++], min = 0;
    unsigned extra = 0;
    if (c < 0x80) continue;
    if (c >= 0xc2 && c <= 0xdf) { extra = 1; c &= 31; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { extra = 2; c &= 15; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { extra = 3; c &= 7; min = 0x10000; }
    else return false;
    if (extra > n - i) return false;
    while (extra--) {
      if ((p[i] & 0xc0) != 0x80) return false;
      c = (c << 6) | (p[i++] & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
  }
  return true;
}
WsParser::WsParser(uint8_t *storage, size_t capacity) : storage_(storage), capacity_(capacity) {}
WsParser::Event WsParser::fail(const char *reason) { error_ = reason; return Event::Error; }
WsParser::Event WsParser::feed(uint8_t b) {
  if (error_) return Event::Error;
  if (messageReady_) { used_ = 0; messageReady_ = false; }
  if (headerUsed_ < headerNeed_) {
    header_[headerUsed_++] = b;
    if (headerUsed_ == 2) {
      final_ = (header_[0] & 0x80) != 0;
      opcode_ = header_[0] & 15;
      if (header_[0] & 0x70) return fail("WS_RSV");
      if (header_[1] & 0x80) return fail("WS_SERVER_MASKED");
      if (opcode_ != 0 && opcode_ != 1 && opcode_ != 2 && opcode_ != 8 && opcode_ != 9 && opcode_ != 10)
        return fail("WS_OPCODE");
      if (opcode_ >= 8 && (!final_ || (header_[1] & 127) > 125)) return fail("WS_CONTROL");
      if (opcode_ == 0 && !fragmented_) return fail("WS_CONTINUATION");
      if ((opcode_ == 1 || opcode_ == 2) && fragmented_) return fail("WS_FRAGMENT_ORDER");
      headerNeed_ = (b & 127) == 126 ? 4 : (b & 127) == 127 ? 10 : 2;
    }
    if (headerUsed_ < headerNeed_) return Event::None;
    length_ = header_[1] & 127;
    if (headerNeed_ > 2) {
      length_ = 0;
      for (size_t i = 2; i < headerNeed_; ++i) length_ = (length_ << 8) | header_[i];
      if ((headerNeed_ == 4 && length_ < 126) ||
          (headerNeed_ == 10 && (length_ < 65536 || (header_[2] & 128)))) return fail("WS_LENGTH");
    }
    if (opcode_ < 8) {
      if (opcode_) messageOpcode_ = opcode_;
      if (!storage_ || capacity_ < 1 || used_ > capacity_ - 1 || length_ > capacity_ - 1 - used_)
        return fail("WS_MESSAGE_LIMIT");
    }
    read_ = 0;
    return length_ ? Event::None : complete();
  }
  if (opcode_ >= 8) control_[read_] = b;
  else storage_[used_++] = b;
  ++read_;
  return read_ == length_ ? complete() : Event::None;
}
WsParser::Event WsParser::complete() {
  headerUsed_ = 0; headerNeed_ = 2;
  if (opcode_ >= 8) {
    eventData_ = control_; eventSize_ = static_cast<size_t>(length_);
    if (opcode_ == 9) return Event::Ping;
    if (opcode_ == 10) return Event::Pong;
    if (length_ == 1) return fail("WS_CLOSE_LENGTH");
    if (length_ >= 2) {
      unsigned code = (unsigned(control_[0]) << 8) | control_[1];
      if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 ||
          code == 1015 || (code >= 1016 && code < 3000)) return fail("WS_CLOSE_CODE");
      if (!validUtf8(control_ + 2, eventSize_ - 2)) return fail("WS_CLOSE_UTF8");
    }
    return Event::Close;
  }
  fragmented_ = !final_;
  if (!final_) return Event::None;
  if (messageOpcode_ == 1 && !validUtf8(storage_, used_)) return fail("WS_UTF8");
  storage_[used_] = 0; eventData_ = storage_; eventSize_ = used_; messageReady_ = true;
  return Event::Message;
}
bool WsParser::finish() {
  if (pending()) fail("WS_TRUNCATED");
  return !error_;
}
size_t wsClientHeader(uint8_t *p, uint8_t opcode, size_t n, const uint8_t mask[4]) {
  p[0] = 0x80 | opcode;
  size_t at = 2;
  if (n < 126) p[1] = 0x80 | uint8_t(n);
  else if (n <= 65535) { p[1] = 0xfe; p[2] = uint8_t(n >> 8); p[3] = uint8_t(n); at = 4; }
  else {
    p[1] = 0xff;
    const uint64_t length = n;
    for (int i = 0; i < 8; ++i) p[2 + i] = uint8_t(length >> (56 - i * 8));
    at = 10;
  }
  memcpy(p + at, mask, 4);
  return at + 4;
}
void wsMask(uint8_t *p, size_t n, const uint8_t mask[4], size_t offset) {
  for (size_t i = 0; i < n; ++i) p[i] ^= mask[(i + offset) & 3];
}
static std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static bool token(const std::string &s, const char *wanted) {
  size_t at = 0;
  while (at < s.size()) {
    size_t end = s.find(',', at);
    if (trim(lower(s.substr(at, end - at))) == wanted) return true;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return false;
}
bool wsUpgradeValid(const std::string &s, const std::string &expected) {
  if (s.size() > 8192 || s.find("\r\n\r\n") != s.size() - 4 || s.find('\0') != std::string::npos) return false;
  size_t end = s.find("\r\n");
  if (end == std::string::npos || (s.substr(0, end) != "HTTP/1.1 101" &&
      s.substr(0, 13) != "HTTP/1.1 101 ")) return false;
  bool upgrade = false, connection = false, accept = false;
  for (size_t at = end + 2; at < s.size() - 2; at = end + 2) {
    end = s.find("\r\n", at);
    if (end == std::string::npos || s[at] == ' ' || s[at] == '\t') return false;
    const std::string line = s.substr(at, end - at);
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string name = lower(line.substr(0, colon)), value = trim(line.substr(colon + 1));
    if (name == "upgrade") { if (upgrade || lower(value) != "websocket") return false; upgrade = true; }
    if (name == "connection") { if (connection || !token(value, "upgrade")) return false; connection = true; }
    if (name == "sec-websocket-accept") { if (accept || value != expected) return false; accept = true; }
    if (name == "sec-websocket-extensions" || name == "sec-websocket-protocol") return false;
  }
  return upgrade && connection && accept;
}
} // namespace Assistant
