#pragma once
#include "platform.h"
#include "ws_codec.h"
#include <string>

namespace Assistant {
class PhoneVoice;
using StopFn = bool (*)(void *);
class LiveSocket {
public:
  LiveSocket(uint8_t *storage, size_t capacity, StopFn stop, void *context);
  ~LiveSocket();
  LiveSocket(const LiveSocket &) = delete;
  LiveSocket &operator=(const LiveSocket &) = delete;
  bool connect(const char *key);
  bool send(const std::string &json);
  bool sendAudio(const uint8_t *pcm, size_t size);
  bool endInput();
  WsParser::Event poll(); // Non-blocking, <= 4096 available bytes per call.
  const uint8_t *data() const;
  size_t size() const;
  bool isPhone() const;
  bool compressedAudio() const { return mediaBitrate_ > 0; }
  const char *mediaName() const;
  uint32_t mediaSentBytes() const;
  uint32_t mediaReceivedBytes() const;
  void codecTiming(uint32_t &encodeUs, uint32_t &decodeUs) const;
  uint32_t networkConnectUs() const { return networkConnectUs_; }
  uint32_t wsUpgradeUs() const { return wsUpgradeUs_; }
  const char *error() const;
  bool inputPending();
  void close();
private:
  bool frame(uint8_t opcode, const uint8_t *data, size_t size, uint32_t timeout = 10000);
  NetworkClient client_;
  PhoneVoice *voice_ = nullptr;
  uint8_t *storage_;
  size_t capacity_;
  int mediaBitrate_ = 0;
  bool connectMedia(const char *key);
  WsParser parser_;
  StopFn stop_;
  void *context_;
  const char *error_ = "Falha no WebSocket.";
  uint32_t frameStarted_ = 0;
  uint32_t networkConnectUs_ = 0, wsUpgradeUs_ = 0;
  uint16_t closeCode_ = 1000;
  bool framePending_ = false, upgraded_ = false, closeSent_ = false;
};
class ToolGateway {
public:
  // One POST per invocation; redirects use GET on a fixed allowlisted TLS host.
  // The owner may reconcile an uncertain outcome using the exact same body/id.
  static std::string execute(const std::string &url, const std::string &payload,
                             const std::string &id, StopFn stop, void *context,
                             uint32_t budgetMs = 25000);
};
} // namespace Assistant
