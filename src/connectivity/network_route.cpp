#include "network_route.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "ble_relay.h"
#include "../net/network_client.h"
#include "../net/http_client.h"
#include "../net/wifi_mgr.h"
#include <atomic>

namespace NetworkRoute {
namespace {
std::atomic<bool> enabled{false}, phone{false};
std::atomic<uint32_t> passkey{0};
std::atomic<DiagnosticRoute> diagnostic{DiagnosticRoute::Auto};
std::atomic<bool> earlyPlayback{true};
std::atomic<DiagnosticMedia> media{kDefaultPhoneMedia};
std::atomic_flag selecting = ATOMIC_FLAG_INIT;
// Abaixo, somente sob Selection; nunca há espera segurando portMUX.
WifiManager *manager = nullptr;
SettingsStore *store = nullptr;
bool wifiFailed = false;
uint32_t wifiFailedAt = 0;
std::atomic<bool> retainPhone{false};
std::atomic<uint32_t> retainPhoneUntil{0};
uint32_t left(uint32_t start, uint32_t budget) {
  uint32_t elapsed = millis() - start;
  return elapsed >= budget ? 0 : budget - elapsed;
}
struct Selection {
  bool held = false;
  explicit Selection(uint32_t timeoutMs) {
    uint32_t start = millis();
    do {
      if (!selecting.test_and_set(std::memory_order_acquire)) { held = true; return; }
      if (!left(start, timeoutMs)) return;
      delay(1);
    } while (true);
  }
  ~Selection() { if (held) selecting.clear(std::memory_order_release); }
  Selection(const Selection &) = delete;
  Selection &operator=(const Selection &) = delete;
};
bool selectPhone(uint32_t timeoutMs) {
  if (!enabled.load(std::memory_order_acquire)) return false;
  if (PhoneRelay::ready()) { phone.store(true, std::memory_order_release); return true; }
  if (!timeoutMs) return false;
  uint32_t start = millis();
  // begin é idempotente durante pareamento. Após disconnect, active() ainda
  // pode ser true, mas a API exige begin explícito para reanunciar o rádio.
  if (!PhoneRelay::begin(passkey.load(std::memory_order_relaxed))) return false;
  uint32_t remaining = left(start, timeoutMs);
  if (!remaining || !PhoneRelay::waitReady(remaining)) return false;
  phone.store(true, std::memory_order_release);
  return true;
}
bool probeWifi(uint32_t timeoutMs) {
  if (timeoutMs < 500) return false;
  uint32_t start = millis();
  NetworkClient probe;
  probe.setInsecure(); // política existente; 204 detecta portal comum, não prova identidade
  // URL de conectividade Google: HTTPS + resposta 204 completa, sem redirects.
  if (!probe.connectDirect("www.google.com", 443, timeoutMs * 4 / 5)) return false;
  static const char request[] = "GET /generate_204 HTTP/1.1\r\nHost: www.google.com\r\nConnection: close\r\n\r\n";
  uint32_t remaining = left(start, timeoutMs);
  if (!remaining) return false;
  probe.setWriteTimeoutMs(remaining);
  size_t sent = 0;
  while (sent < sizeof(request) - 1) {
    remaining = left(start, timeoutMs);
    if (!remaining || !probe.connected()) return false;
    probe.setWriteTimeoutMs(remaining);
    size_t written = probe.write(reinterpret_cast<const uint8_t *>(request) + sent, sizeof(request) - 1 - sent);
    if (written > sizeof(request) - 1 - sent) return false;
    sent += written;
    if (!written) delay(1);
  }
  remaining = left(start, timeoutMs);
  HttpResponse response;
  return remaining && HttpClient::readResponse(probe, response, 0, remaining) &&
         response.statusCode == 204 && response.headersComplete && response.bodyComplete;
}
bool selectRoute(uint32_t timeoutMs) {
  uint32_t start = millis();
  const DiagnosticRoute policy = diagnostic.load(std::memory_order_acquire);
  if (policy == DiagnosticRoute::PhoneOnly) return selectPhone(timeoutMs);
  if (policy == DiagnosticRoute::WifiOnly) {
    bool associated = WiFi.status() == WL_CONNECTED;
    if (!associated && manager && store)
      associated = manager->connect(*store, min(left(start, timeoutMs), uint32_t(5000)));
    phone.store(false, std::memory_order_release);
    return associated;
  }
  bool readyPhone = enabled.load(std::memory_order_acquire) && PhoneRelay::ready();
  bool associated = WiFi.status() == WL_CONNECTED;
  // Telefone pronto: só testar Wi-Fi já associado/conhecido. Não perder um
  // telefone válido em scan a cada fala; após falha, cooldown de 45 segundos.
  bool cooldown = wifiFailed && millis() - wifiFailedAt < 45000;
  if (readyPhone && cooldown) {
    phone.store(true, std::memory_order_release); return true;
  }
  if (!associated && !readyPhone && manager && store) {
    associated = manager->connect(*store, min(left(start, timeoutMs), uint32_t(5000)));
  }
  if (associated && probeWifi(min(left(start, timeoutMs), uint32_t(4000)))) {
    wifiFailed = false;
    phone.store(false, std::memory_order_release);
    return true; // BLE pronto permanece intacto, inclusive seus canais abertos
  }
  // Sem celular habilitado, não bloquear a sincronização antiga só porque o
  // endpoint opcional de diagnóstico está filtrado/lento nessa rede.
  if (associated && !enabled.load(std::memory_order_acquire)) {
    phone.store(false, std::memory_order_release); return true;
  }
  wifiFailed = true; wifiFailedAt = millis();
  return selectPhone(left(start, timeoutMs));
}
}
void configure(const Settings &settings) {
  enabled.store(settings.bleFallbackEnabled && settings.blePasskey <= 999999,
                std::memory_order_release);
  passkey.store(settings.blePasskey, std::memory_order_relaxed);
  phone.store(false, std::memory_order_release);
  manager = nullptr; store = nullptr;
  // Preserva cooldown entre sessões/falas.
}
bool connect(WifiManager &wifi, SettingsStore &settings, uint32_t timeoutMs) {
  uint32_t start = millis();
  Selection lock(timeoutMs);
  if (!lock.held) return false;
  retainPhone.store(false, std::memory_order_release); // The awake retention window is now owned by this session.
  manager = &wifi; store = &settings; // donos persistem até finish()
  return selectRoute(left(start, timeoutMs));
}
bool online() {
  return phone.load(std::memory_order_acquire) ? PhoneRelay::ready() : WiFi.status() == WL_CONNECTED;
}
bool phoneSelected() { return phone.load(std::memory_order_acquire); }
bool fallbackEnabled() { return enabled.load(std::memory_order_acquire); }
const char *label() { return online() ? (phoneSelected() ? "Telefone BLE" : "Wi-Fi") : "offline"; }
bool fallbackToPhone(uint32_t timeoutMs) {
  const DiagnosticRoute policy = diagnostic.load(std::memory_order_acquire);
  if (policy == DiagnosticRoute::WifiOnly) return false;
  uint32_t start = millis();
  Selection lock(timeoutMs);
  if (!lock.held) return false;
  if (phone.load(std::memory_order_acquire) && PhoneRelay::ready()) return true;
  wifiFailed = true; wifiFailedAt = millis();
  return selectPhone(left(start, timeoutMs));
}
bool recover(uint32_t timeoutMs) {
  if (online()) return true;
  uint32_t start = millis();
  Selection lock(timeoutMs);
  if (!lock.held) return false;
  if (online()) return true;
  // Só chegamos aqui após queda real. selectRoute preserva telefone válido e
  // pode tentar Wi-Fi novamente quando o telefone desapareceu.
  return selectRoute(left(start, timeoutMs));
}
void setDiagnosticRoute(DiagnosticRoute route) { diagnostic.store(route, std::memory_order_release); }
DiagnosticRoute diagnosticRoute() { return diagnostic.load(std::memory_order_acquire); }
void setDiagnosticEarlyPlayback(bool enabled) { earlyPlayback.store(enabled, std::memory_order_release); }
bool diagnosticEarlyPlayback() { return earlyPlayback.load(std::memory_order_acquire); }
void setDiagnosticMedia(DiagnosticMedia value) { media.store(value, std::memory_order_release); }
DiagnosticMedia diagnosticMedia() { return media.load(std::memory_order_acquire); }
void finishRetainingPhone(uint32_t timeoutMs) {
  while (selecting.test_and_set(std::memory_order_acquire)) delay(1);
  const bool keep = PhoneRelay::ready() && timeoutMs;
  phone.store(false, std::memory_order_release);
  manager = nullptr; store = nullptr;
  diagnostic.store(DiagnosticRoute::Auto, std::memory_order_release);
  earlyPlayback.store(true, std::memory_order_release);
  media.store(kDefaultPhoneMedia, std::memory_order_release);
  if (keep) {
    retainPhoneUntil.store(millis() + min(timeoutMs, uint32_t(30000)), std::memory_order_release);
    retainPhone.store(true, std::memory_order_release);
  } else {
    retainPhone.store(false, std::memory_order_release);
    PhoneRelay::stop();
  }
  selecting.clear(std::memory_order_release);
}
void service() {
  if (!retainPhone.load(std::memory_order_acquire) ||
      static_cast<int32_t>(millis() - retainPhoneUntil.load(std::memory_order_acquire)) < 0) return;
  while (selecting.test_and_set(std::memory_order_acquire)) delay(1);
  if (retainPhone.load(std::memory_order_acquire) &&
      static_cast<int32_t>(millis() - retainPhoneUntil.load(std::memory_order_acquire)) >= 0) {
    retainPhone.store(false, std::memory_order_release);
    PhoneRelay::stop();
  }
  selecting.clear(std::memory_order_release);
}
void finish() {
  // Precondição: workers encerrados e sockets fechados, inclusive Live/gateway.
  while (selecting.test_and_set(std::memory_order_acquire)) delay(1);
  PhoneRelay::stop();
  phone.store(false, std::memory_order_release);
  diagnostic.store(DiagnosticRoute::Auto, std::memory_order_release);
  earlyPlayback.store(true, std::memory_order_release);
  media.store(kDefaultPhoneMedia, std::memory_order_release);
  retainPhone.store(false, std::memory_order_release);
  manager = nullptr; store = nullptr;
  selecting.clear(std::memory_order_release);
}
}
#endif
