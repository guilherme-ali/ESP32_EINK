#pragma once
#include "platform.h"
#include "ws_codec.h"
#include <string>

namespace Assistant {
using StopFn = bool (*)(void *);
class LiveSocket {
public:
  LiveSocket(uint8_t *storage, size_t capacity, StopFn stop, void *context);
  bool connect(const char *key);
  bool send(const std::string &json);
  WsParser::Event poll(); // Non-blocking, <= 4096 available bytes per call.
  const uint8_t *data() const { return parser_.data(); }
  size_t size() const { return parser_.size(); }
  const char *error() const { return error_; }
  bool inputPending() { return client_.available() > 0 || parser_.pending(); }
  void close();
private:
  bool frame(uint8_t opcode, const uint8_t *data, size_t size, uint32_t timeout = 10000);
  NetworkClient client_;
  WsParser parser_;
  StopFn stop_;
  void *context_;
  const char *error_ = "Falha no WebSocket.";
  uint32_t frameStarted_ = 0;
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
