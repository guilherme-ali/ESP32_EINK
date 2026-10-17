#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <deque>
#include <string>
#include <vector>

// Somente fronteiras SDK. NetworkClient é SEMPRE o .cpp de produção no modo
// ESP32 destes testes, nunca o alias host. Nenhum parser/roteador copiado aqui.
namespace RouteTest {
inline bool nativeSuccess = true;
inline uint32_t nativeCost = 0;
inline size_t nativeWriteLimit = SIZE_MAX;
inline std::vector<std::string> nativeHosts;
inline std::string nativeBytes;
inline int timeout = 0;
inline unsigned long handshake = 0;
inline const char *ca = nullptr;
inline bool insecure = false;
}
struct sslclient_context {
  unsigned long handshake_timeout = 120000;
  unsigned long socket_timeout = 30000;
};
class WiFiClientSecure {
protected:
  sslclient_context context_;
  sslclient_context *sslclient = &context_;
  int _timeout = 30000;
  const char *_CA_cert = nullptr, *_cert = nullptr, *_private_key = nullptr;
  bool up_ = false;
public:
  virtual ~WiFiClientSecure() = default;
  virtual int connect(const char *, uint16_t) { return 0; }
  virtual int connect(const char *, uint16_t, int32_t) { return 0; }
  int connect(IPAddress, uint16_t, const char *host, const char *root, const char *, const char *) {
    RouteTest::nativeHosts.emplace_back(host);
    RouteTest::timeout = _timeout;
    RouteTest::handshake = sslclient->handshake_timeout;
    RouteTest::ca = root;
    delay(RouteTest::nativeCost);
    up_ = RouteTest::nativeSuccess;
    return up_;
  }
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int read(uint8_t *, size_t) { return -1; }
  virtual int peek() { return -1; }
  virtual size_t write(uint8_t value) { return write(&value, 1); }
  virtual size_t write(const uint8_t *data, size_t count) {
    if (!up_) return 0;
    size_t accepted = min(count, RouteTest::nativeWriteLimit);
    RouteTest::nativeBytes.append(reinterpret_cast<const char *>(data), accepted);
    return accepted;
  }
  virtual uint8_t connected() { return up_; }
  virtual void stop() { up_ = false; }
  virtual void flush() {}
  virtual int setTimeout(uint32_t seconds) { _timeout = seconds * 1000; return 0; }
  void setInsecure() { RouteTest::insecure = true; }
  void setCACert(const char *ca) { _CA_cert = ca; }
  void setHandshakeTimeout(unsigned long seconds) { sslclient->handshake_timeout = seconds * 1000; }
};
