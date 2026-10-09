#pragma once
#include "Arduino.h"
#include <deque>
#include <functional>
#include <vector>

namespace Host {
struct Wire {
  std::string bytes;
  size_t fragment = SIZE_MAX;
  uint32_t gapMs = 0;
  bool keepOpen = false;
  size_t writeLimit = SIZE_MAX;
  bool waitForBody = false;
};
inline std::deque<Wire> responses;
inline std::vector<std::string> requests;
inline std::function<void(size_t)> onConnect;
inline void resetNetwork() { responses.clear(); requests.clear(); onConnect = nullptr; }
inline void enqueue(const std::string &bytes, size_t fragment = SIZE_MAX,
                    uint32_t gapMs = 0, bool keepOpen = false) {
  bool waitForBody = bytes.find(" 200 ") != std::string::npos || bytes.find(" 201 ") != std::string::npos;
  responses.push_back({bytes, fragment, gapMs, keepOpen, SIZE_MAX, waitForBody});
}
}

// A scheduled byte stream, deliberately ignorant of HTTP/JSON semantics.
class WiFiClientSecure {
  Host::Wire wire_;
  size_t position_ = 0;
  size_t released_ = 0;
  uint32_t readyAt_ = 0;
  bool stopped_ = false;
  size_t requestIndex_ = SIZE_MAX;

public:
  std::string sent;
  bool stopped() const { return stopped_; }
  size_t consumed() const { return position_; }
  void feed(Host::Wire wire) {
    wire_ = std::move(wire);
    position_ = released_ = 0;
    readyAt_ = millis(); stopped_ = false;
  }
  int available() {
    if (stopped_) return 0;
    if (wire_.waitForBody && requestIndex_ != SIZE_MAX) {
      size_t end = sent.find("\r\n\r\n");
      size_t length = sent.find("\r\nContent-Length: ");
      if (end == std::string::npos) return 0;
      if (length != std::string::npos && length < end) {
        size_t declared = static_cast<size_t>(std::strtoull(sent.c_str() + length + 18, nullptr, 10));
        if (sent.size() < end + 4 + declared) return 0;
      }
    }
    if (position_ == released_ && position_ < wire_.bytes.size() &&
        static_cast<int32_t>(millis() - readyAt_) >= 0)
      released_ += std::min(wire_.fragment, wire_.bytes.size() - released_);
    return static_cast<int>(released_ - position_);
  }
  bool connected() {
    return !stopped_ && (position_ < wire_.bytes.size() || wire_.keepOpen);
  }
  int read() {
    if (!available()) return -1;
    int result = static_cast<uint8_t>(wire_.bytes[position_++]);
    if (position_ == released_) readyAt_ = millis() + wire_.gapMs;
    return result;
  }
  int read(uint8_t *out, size_t count) {
    size_t n = std::min(count, static_cast<size_t>(available()));
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(read());
    return static_cast<int>(n);
  }
  size_t write(const uint8_t *data, size_t count) {
    if (stopped_) return 0;
    size_t n = std::min(count, wire_.writeLimit);
    sent.append(reinterpret_cast<const char *>(data), n);
    if (requestIndex_ != SIZE_MAX) Host::requests[requestIndex_] = sent;
    return n;
  }
  size_t print(const String &value) {
    return write(reinterpret_cast<const uint8_t *>(value.c_str()), value.length());
  }
  bool connect(const char *, uint16_t, int32_t = 0) {
    if (Host::responses.empty()) return false;
    if (Host::onConnect) Host::onConnect(Host::requests.size());
    feed(std::move(Host::responses.front())); Host::responses.pop_front();
    requestIndex_ = Host::requests.size(); Host::requests.emplace_back();
    return true;
  }
  void stop() { stopped_ = true; }
  void setInsecure() {}
  void setHandshakeTimeout(unsigned long) {}
  void setTimeout(unsigned long) {}
};
