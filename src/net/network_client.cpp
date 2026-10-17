#include "network_client.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "../connectivity/ble_relay.h"
#include "../connectivity/network_route.h"
#include <WiFi.h>
#include <atomic>
#include <cstring>
#include <lwip/dns.h>
#include <lwip/tcpip.h>

namespace {
uint32_t left(uint32_t start, uint32_t budget) {
  uint32_t elapsed = millis() - start;
  return elapsed >= budget ? 0 : budget - elapsed;
}
// Contextos estáticos: callbacks DNS podem sobreviver ao prazo e ao socket.
// Um slot só é reutilizado depois do callback E da saída do seu chamador.
// Live/gateway/Drive podem resolver em paralelo, sem lock durante a espera.
struct Lookup {
  char host[256] = {};
  uint32_t address = 0;
  std::atomic<bool> owned{false};
  std::atomic<bool> done{true};
} lookups[4];
void dnsFound(const char *, const ip_addr_t *address, void *arg) {
  auto &lookup = *static_cast<Lookup *>(arg);
  lookup.address = address && IP_IS_V4(address) ? ip4_addr_get_u32(ip_2_ip4(address)) : 0;
  lookup.done.store(true, std::memory_order_release);
}
void dnsStart(void *arg) {
  auto &lookup = *static_cast<Lookup *>(arg);
  ip_addr_t address;
  err_t result = dns_gethostbyname_addrtype(lookup.host, &address, dnsFound, arg, LWIP_DNS_ADDRTYPE_IPV4);
  if (result == ERR_OK) dnsFound(nullptr, &address, arg);
  else if (result != ERR_INPROGRESS) dnsFound(nullptr, nullptr, arg);
}
bool resolveWithin(const char *host, IPAddress &address, uint32_t timeoutMs) {
  if (!timeoutMs || strlen(host) >= sizeof(lookups[0].host)) return false;
  for (auto &lookup : lookups) {
    bool expected = false;
    if (!lookup.owned.compare_exchange_strong(expected, true, std::memory_order_acquire)) continue;
    if (!lookup.done.load(std::memory_order_acquire)) {
      lookup.owned.store(false, std::memory_order_release); continue;
    }
    memcpy(lookup.host, host, strlen(host) + 1);
    lookup.done.store(false, std::memory_order_relaxed);
    if (tcpip_try_callback(dnsStart, &lookup) != ERR_OK) {
      lookup.done.store(true, std::memory_order_release);
      lookup.owned.store(false, std::memory_order_release); return false;
    }
    uint32_t start = millis();
    while (!lookup.done.load(std::memory_order_acquire) && left(start, timeoutMs)) delay(1);
    bool done = lookup.done.load(std::memory_order_acquire);
    if (done) address = lookup.address;
    lookup.owned.store(false, std::memory_order_release);
    return done && static_cast<uint32_t>(address) != 0;
  }
  return false; // slots ocupados por callbacks antigos: falhar fechado, sem DNS bloqueante
}
}

NetworkClient::~NetworkClient() { stop(); }
int NetworkClient::connect(const char *host, uint16_t port) {
  return connect(host, port, 15000);
}
int NetworkClient::connect(const char *host, uint16_t port, int32_t timeoutMs) {
  stop();
  if (!host || !*host || !port || timeoutMs <= 0) return 0;
  uint32_t budget = static_cast<uint32_t>(timeoutMs), start = millis();
  if (NetworkRoute::phoneSelected()) return connectPhone(host, port, budget);
  if (WiFi.status() == WL_CONNECTED) {
    // Reserva tempo para pareamento/open; DNS, TCP e TLS dividem o mesmo prazo.
    uint32_t nativeBudget = NetworkRoute::fallbackEnabled() ? min(budget / 2, uint32_t(5000)) : budget;
    if (connectDirect(host, port, nativeBudget)) return 1;
  }
  WiFiClientSecure::stop();
  // Somente connect pode trocar rota: ainda não há headers/body de aplicação.
  uint32_t remaining = left(start, budget);
  if (!remaining || !NetworkRoute::fallbackToPhone(remaining)) return 0;
  return connectPhone(host, port, left(start, budget));
}
int NetworkClient::connectDirect(const char *host, uint16_t port, uint32_t timeoutMs) {
  stop();
  if (!host || !*host || !port || !timeoutMs || !sslclient || WiFi.status() != WL_CONNECTED) return 0;
  uint32_t start = millis();
  IPAddress address;
  if (!resolveWithin(host, address, timeoutMs / 3)) return 0;
  uint32_t remaining = left(start, timeoutMs);
  if (remaining < 300) return 0;
  // Core 2.0.17: TCP e handshake são ms distintos. Margem para setup/scheduler.
  int previousTimeout = _timeout;
  auto previousHandshake = sslclient->handshake_timeout;
  _timeout = (remaining - 250) / 2;
  sslclient->handshake_timeout = min(static_cast<unsigned long>((remaining - 250) / 2),
                                   static_cast<unsigned long>(previousHandshake));
  int result = WiFiClientSecure::connect(address, port, host, _CA_cert, _cert, _private_key);
  _timeout = previousTimeout;
  sslclient->handshake_timeout = previousHandshake;
  if (!left(start, timeoutMs)) { stop(); return 0; }
  return result;
}
int NetworkClient::connectPhone(const char *host, uint16_t port, uint32_t timeoutMs) {
  phone_ = true;
  uint32_t start = millis();
  if (!timeoutMs || !NetworkRoute::fallbackToPhone(timeoutMs)) return 0;
  uint32_t remaining = left(start, timeoutMs);
  if (!remaining) return 0;
  channel_ = PhoneRelay::open(host, port, remaining); // DNS e TLS somente no Android
  if (channel_ >= 0 && !left(start, timeoutMs)) {
    PhoneRelay::close(channel_); channel_ = -1;
  }
  return channel_ >= 0;
}
int NetworkClient::available() {
  return phone_ ? (channel_ >= 0 ? PhoneRelay::available(channel_) : 0) : WiFiClientSecure::available();
}
int NetworkClient::read() {
  uint8_t value;
  int result = read(&value, 1);
  return result == 1 ? value : -1;
}
int NetworkClient::read(uint8_t *buffer, size_t size) {
  if (!buffer && size) return -1;
  return phone_ ? (channel_ >= 0 ? PhoneRelay::read(channel_, buffer, size) : -1) :
                  WiFiClientSecure::read(buffer, size);
}
int NetworkClient::peek() {
  return phone_ ? (channel_ >= 0 ? PhoneRelay::peek(channel_) : -1) : WiFiClientSecure::peek();
}
size_t NetworkClient::write(uint8_t value) { return write(&value, 1); }
size_t NetworkClient::write(const uint8_t *buffer, size_t size) {
  if (!buffer && size) return 0;
  // Mesmo um write parcial/interrompido nunca reconecta nem repete o prefixo.
  return phone_ ? (channel_ >= 0 ? PhoneRelay::write(channel_, buffer, size, writeTimeoutMs_) : 0) :
                  WiFiClientSecure::write(buffer, size);
}
uint8_t NetworkClient::connected() {
  return phone_ ? channel_ >= 0 && PhoneRelay::connected(channel_) : WiFiClientSecure::connected();
}
void NetworkClient::stop() {
  if (channel_ >= 0) { PhoneRelay::close(channel_); channel_ = -1; }
  phone_ = false;
  WiFiClientSecure::stop();
}
void NetworkClient::flush() {}
int NetworkClient::setTimeout(uint32_t seconds) {
  writeTimeoutMs_ = seconds > UINT32_MAX / 1000 ? UINT32_MAX : seconds * 1000;
  return phone_ ? 0 : WiFiClientSecure::setTimeout(seconds);
}
void NetworkClient::setWriteTimeoutMs(uint32_t milliseconds) {
  writeTimeoutMs_ = milliseconds;
  if (!phone_ && sslclient) sslclient->socket_timeout = milliseconds;
}
#endif
