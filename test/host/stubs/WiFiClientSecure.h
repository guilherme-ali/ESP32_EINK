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
  size_t disconnectAfterRead = SIZE_MAX;
};
inline std::deque<Wire> responses;
inline std::vector<std::string> requests;
inline std::function<void(size_t)> onConnect;
inline std::vector<std::string> connectionHosts;
inline unsigned stops = 0;
// Hook por REQUEST, inclusive quando nao existe um novo handshake TLS.
inline void resetNetwork() {
  responses.clear(); requests.clear(); connectionHosts.clear(); stops = 0; onConnect = nullptr;
}
inline void enqueue(const std::string &bytes, size_t fragment = SIZE_MAX,
                    uint32_t gapMs = 0, bool keepOpen = false) {
  int status = bytes.size() >= 12 ? std::atoi(bytes.c_str() + 9) : 0;
  bool waitForBody = status < 400;
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
  bool requestComplete() const {
    size_t end = sent.find("\r\n\r\n"), length = sent.find("\r\nContent-Length: ");
    if (end == std::string::npos) return false;
    if (length == std::string::npos || length >= end) return true;
    size_t declared = static_cast<size_t>(std::strtoull(sent.c_str() + length + 18, nullptr, 10));
    return sent.size() >= end + 4 + declared;
  }
  bool nextRequest() {
    if (Host::responses.empty()) { stop(); return false; }
    if (Host::onConnect) Host::onConnect(Host::requests.size());
    feed(std::move(Host::responses.front())); Host::responses.pop_front();
    sent.clear(); requestIndex_ = Host::requests.size(); Host::requests.emplace_back();
    return true;
  }

public:
  ~WiFiClientSecure() { stop(); }
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
    if (wire_.waitForBody && requestIndex_ != SIZE_MAX && !requestComplete()) return 0;
    if (position_ >= wire_.disconnectAfterRead) { stop(); return 0; }
    if (position_ == released_ && position_ < wire_.bytes.size() &&
        static_cast<int32_t>(millis() - readyAt_) >= 0)
      released_ += std::min({wire_.fragment, wire_.bytes.size() - released_,
                            wire_.disconnectAfterRead - released_});
    return static_cast<int>(released_ - position_);
  }
  bool connected() {
    return !stopped_ && position_ < wire_.disconnectAfterRead &&
           ((wire_.waitForBody && requestIndex_ != SIZE_MAX && !requestComplete()) ||
            position_ < wire_.bytes.size() || wire_.keepOpen);
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
    // Uma resposta por request, nao por socket. Nao disponibilizar a proxima
    // resposta em available(): isso pareceria erro antecipado do request velho.
    if (requestIndex_ != SIZE_MAX && position_ == wire_.bytes.size() && requestComplete()) {
      const std::string prefix(reinterpret_cast<const char *>(data), std::min(count, size_t(8)));
      bool header = prefix.find("GET ") == 0 || prefix.find("POST ") == 0 ||
                    prefix.find("PUT ") == 0 || prefix.find("PATCH ") == 0 || prefix.find("DELETE ") == 0;
      if (header && !nextRequest()) return 0;
    }
    size_t n = std::min(count, wire_.writeLimit);
    sent.append(reinterpret_cast<const char *>(data), n);
    if (requestIndex_ != SIZE_MAX) Host::requests[requestIndex_] = sent;
    return n;
  }
  size_t print(const String &value) {
    return write(reinterpret_cast<const uint8_t *>(value.c_str()), value.length());
  }
  bool connect(const char *host, uint16_t, int32_t = 0) {
    if (Host::responses.empty()) return false;
    Host::connectionHosts.emplace_back(host);
    return nextRequest();
  }
  void stop() { if (!stopped_) ++Host::stops; stopped_ = true; }
  void setInsecure() {}
  void setHandshakeTimeout(unsigned long) {}
  void setTimeout(unsigned long) {}
};
