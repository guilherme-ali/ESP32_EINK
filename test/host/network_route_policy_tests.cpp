#include "../../src/connectivity/network_route.h"
#include "../../src/connectivity/ble_relay.h"
#include "../../src/net/http_client.h"
#include "../../src/net/wifi_mgr.h"
#include <cstdio>
#include <cstdlib>
#include <iterator>

#if !defined(ARDUINO_ARCH_ESP32)
#error "Compile with ARDUINO_ARCH_ESP32 and the real network_route.cpp"
#endif

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)

namespace {
bool relayReady = false, relayConnects = true, wifiConnects = true;
unsigned begins = 0, waits = 0, stops = 0, associations = 0, probes = 0;
uint32_t lastPasskey = 0;
}

// Only radio, association and HTTPS probe boundaries are faked. All route,
// diagnostic media and retention decisions come from network_route.cpp.
namespace PhoneRelay {
bool begin(uint32_t passkey) { ++begins; lastPasskey = passkey; return true; }
bool waitReady(uint32_t timeoutMs) {
  CHECK(timeoutMs > 0);
  ++waits;
  return relayReady = relayConnects;
}
bool ready() { return relayReady; }
void stop() { ++stops; relayReady = false; }
}

bool WifiManager::connect(SettingsStore &, uint32_t timeoutMs) {
  CHECK(timeoutMs > 0);
  ++associations;
  WiFi.state = wifiConnects ? WL_CONNECTED : 0;
  return wifiConnects;
}

NetworkClient::~NetworkClient() = default;
int NetworkClient::connect(const char *, uint16_t) { std::abort(); }
int NetworkClient::connect(const char *, uint16_t, int32_t) { std::abort(); }
int NetworkClient::connectDirect(const char *host, uint16_t port, uint32_t timeoutMs) {
  CHECK(std::strcmp(host, "www.google.com") == 0 && port == 443 && timeoutMs > 0);
  ++probes;
  up_ = true;
  return 1;
}
int NetworkClient::available() { return 0; }
int NetworkClient::read() { return -1; }
int NetworkClient::read(uint8_t *, size_t) { return -1; }
int NetworkClient::peek() { return -1; }
size_t NetworkClient::write(uint8_t value) { return write(&value, 1); }
size_t NetworkClient::write(const uint8_t *data, size_t size) {
  return WiFiClientSecure::write(data, size);
}
uint8_t NetworkClient::connected() { return up_; }
void NetworkClient::stop() { up_ = false; }
void NetworkClient::flush() {}
int NetworkClient::setTimeout(uint32_t seconds) { return WiFiClientSecure::setTimeout(seconds); }
void NetworkClient::setWriteTimeoutMs(uint32_t milliseconds) { CHECK(milliseconds > 0); }
bool HttpClient::readResponse(NetworkClient &, HttpResponse &out, size_t maxBody, uint32_t timeoutMs) {
  CHECK(maxBody == 0 && timeoutMs > 0);
  out.statusCode = 204;
  out.headersComplete = out.bodyComplete = true;
  return true;
}

namespace {
void checkDefaults() {
  CHECK(NetworkRoute::diagnosticMedia() == DiagnosticMedia::Opus48);
  CHECK(NetworkRoute::diagnosticRoute() == DiagnosticRoute::Auto);
  CHECK(NetworkRoute::diagnosticEarlyPlayback());
  CHECK(!NetworkRoute::phoneSelected());
}

void reset(SettingsStore &settings) {
  NetworkRoute::finish();
  Host::resetClock();
  relayReady = false;
  relayConnects = wifiConnects = true;
  begins = waits = stops = associations = probes = 0;
  WiFi.state = WL_CONNECTED;
  RouteTest::nativeBytes.clear();
  settings.get().bleFallbackEnabled = true;
  settings.get().blePasskey = 123456;
  NetworkRoute::configure(settings.get());
}

void boot_defaults_to_opus48() {
  // Must run before reset()/finish(): this checks the production atomic init.
  CHECK(NetworkRoute::kDefaultPhoneMedia == DiagnosticMedia::Opus48);
  checkDefaults();
  CHECK(stops == 0);
  Settings settings;
  NetworkRoute::configure(settings);
  checkDefaults();
}

void finish_restores_volatile_overrides() {
  SettingsStore settings;
  for (auto media : {DiagnosticMedia::Legacy, DiagnosticMedia::Opus64}) {
    reset(settings);
    NetworkRoute::setDiagnosticMedia(media);
    NetworkRoute::setDiagnosticRoute(DiagnosticRoute::PhoneOnly);
    NetworkRoute::setDiagnosticEarlyPlayback(false);
    CHECK(NetworkRoute::diagnosticMedia() == media);
    CHECK(!NetworkRoute::diagnosticEarlyPlayback());
    NetworkRoute::finish();
    checkDefaults();
    CHECK(stops == 1);
    // A new ordinary session must not inherit a benchmark's media override.
    NetworkRoute::configure(settings.get());
    checkDefaults();
  }
}

void retained_phone_restores_media_and_is_reusable() {
  WifiManager wifi;
  SettingsStore settings;
  for (auto media : {DiagnosticMedia::Legacy, DiagnosticMedia::Opus64}) {
    reset(settings);
    NetworkRoute::setDiagnosticRoute(DiagnosticRoute::PhoneOnly);
    NetworkRoute::setDiagnosticMedia(media);
    NetworkRoute::setDiagnosticEarlyPlayback(false);
    CHECK(NetworkRoute::connect(wifi, settings, 1000));
    CHECK(NetworkRoute::phoneSelected() && NetworkRoute::online());
    CHECK(NetworkRoute::diagnosticMedia() == media);
    CHECK(begins == 1 && waits == 1 && lastPasskey == 123456);
    NetworkRoute::finishRetainingPhone(30000);
    checkDefaults();
    CHECK(relayReady && stops == 0);
    delay(29999);
    NetworkRoute::service();
    CHECK(relayReady && stops == 0);
    NetworkRoute::setDiagnosticRoute(DiagnosticRoute::PhoneOnly);
    CHECK(NetworkRoute::connect(wifi, settings, 1000));
    CHECK(NetworkRoute::phoneSelected());
    CHECK(NetworkRoute::diagnosticMedia() == DiagnosticMedia::Opus48);
    delay(1);
    NetworkRoute::service();
    CHECK(relayReady && stops == 0 && begins == 1 && waits == 1);
    CHECK(associations == 0 && probes == 0);
    NetworkRoute::finish();
  }
}

void finish_cancels_retention() {
  SettingsStore settings;
  reset(settings);
  relayReady = true;
  NetworkRoute::setDiagnosticMedia(DiagnosticMedia::Opus64);
  NetworkRoute::finishRetainingPhone(30000);
  CHECK(relayReady && stops == 0);
  NetworkRoute::finish();
  checkDefaults();
  CHECK(!relayReady && stops == 1);
  // An expired old retention timer must not stop a subsequent radio session.
  relayReady = true;
  delay(30000);
  NetworkRoute::service();
  CHECK(relayReady && stops == 1);
}

void retention_is_bounded_and_wrap_safe() {
  SettingsStore settings;
  reset(settings);
  Host::resetClock(UINT32_MAX - 10000);
  relayReady = true;
  NetworkRoute::setDiagnosticMedia(DiagnosticMedia::Legacy);
  NetworkRoute::finishRetainingPhone(60000);
  checkDefaults();
  delay(29999);
  NetworkRoute::service();
  CHECK(relayReady && stops == 0);
  delay(1);
  NetworkRoute::service();
  CHECK(!relayReady && stops == 1);
  NetworkRoute::service();
  CHECK(stops == 1);
}

void retention_without_ready_phone_or_budget_stops() {
  SettingsStore settings;
  for (bool ready : {false, true}) {
    reset(settings);
    relayReady = ready;
    NetworkRoute::setDiagnosticMedia(DiagnosticMedia::Legacy);
    NetworkRoute::finishRetainingPhone(ready ? 0 : 30000);
    checkDefaults();
    CHECK(!relayReady && stops == 1);
    delay(30000);
    NetworkRoute::service();
    CHECK(stops == 1);
  }
}

void wifi_only_keeps_phone_out_of_connect_fallback_and_recover() {
  WifiManager wifi;
  SettingsStore settings;
  reset(settings);
  relayReady = true;
  WiFi.state = 0;
  NetworkRoute::setDiagnosticRoute(DiagnosticRoute::WifiOnly);
  CHECK(NetworkRoute::connect(wifi, settings, 1000));
  CHECK(!NetworkRoute::phoneSelected() && NetworkRoute::online());
  CHECK(associations == 1 && probes == 0 && begins == 0);
  CHECK(!NetworkRoute::fallbackToPhone(1000));
  WiFi.state = 0;
  wifiConnects = false;
  CHECK(!NetworkRoute::recover(1000));
  CHECK(!NetworkRoute::online() && !NetworkRoute::phoneSelected());
  wifiConnects = true;
  CHECK(NetworkRoute::recover(1000));
  CHECK(associations == 3 && probes == 0 && begins == 0 && waits == 0);
  CHECK(NetworkRoute::diagnosticMedia() == DiagnosticMedia::Opus48);
}

void phone_only_skips_healthy_wifi_and_recovers_phone() {
  WifiManager wifi;
  SettingsStore settings;
  reset(settings);
  NetworkRoute::setDiagnosticRoute(DiagnosticRoute::PhoneOnly);
  CHECK(NetworkRoute::connect(wifi, settings, 1000));
  CHECK(NetworkRoute::phoneSelected() && NetworkRoute::online());
  CHECK(begins == 1 && waits == 1);
  relayReady = false;
  CHECK(NetworkRoute::recover(1000));
  CHECK(NetworkRoute::phoneSelected() && NetworkRoute::online());
  CHECK(begins == 2 && waits == 2 && associations == 0 && probes == 0);
  CHECK(NetworkRoute::diagnosticMedia() == DiagnosticMedia::Opus48);
}

void auto_keeps_healthy_wifi_and_default_media() {
  WifiManager wifi;
  SettingsStore settings;
  reset(settings);
  relayReady = true;
  CHECK(NetworkRoute::connect(wifi, settings, 5000));
  CHECK(!NetworkRoute::phoneSelected() && NetworkRoute::online());
  CHECK(probes == 1 && associations == 0 && begins == 0 && relayReady);
  CHECK(RouteTest::nativeBytes.find("GET /generate_204 HTTP/1.1\r\n") == 0);
  CHECK(NetworkRoute::diagnosticMedia() == DiagnosticMedia::Opus48);
}
}

int main() {
  void (*tests[])() = {boot_defaults_to_opus48, finish_restores_volatile_overrides,
    retained_phone_restores_media_and_is_reusable, finish_cancels_retention,
    retention_is_bounded_and_wrap_safe, retention_without_ready_phone_or_budget_stops,
    wifi_only_keeps_phone_out_of_connect_fallback_and_recover,
    phone_only_skips_healthy_wifi_and_recovers_phone, auto_keeps_healthy_wifi_and_default_media};
  for (auto test : tests) test();
  NetworkRoute::finish();
  std::printf("NetworkRoute ESP32 real + boundary fakes: %zu/%zu OK\n", std::size(tests), std::size(tests));
}
