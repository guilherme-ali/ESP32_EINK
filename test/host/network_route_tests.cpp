#include "../../src/net/network_client.h"
#include "../../src/net/http_client.h"
#include "../../src/connectivity/network_route.h"
#include "../../src/connectivity/ble_relay.h"
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#if !defined(ARDUINO_ARCH_ESP32)
#error "Este teste exige a implementação ESP32 real de NetworkClient, não o alias host"
#endif
static_assert(!std::is_same<NetworkClient, WiFiClientSecure>::value, "testando alias");
static_assert(!std::is_copy_constructible<NetworkClient>::value, "canal não pode ser copiado");
static_assert(!std::is_move_constructible<NetworkClient>::value, "canal não pode ser movido");
static_assert(std::has_virtual_destructor<NetworkClient>::value, "destrutor deve ser virtual");

namespace RouteTest {
bool enabled = true, phone = false, ready = true, allowFallback = true;
DiagnosticRoute route = DiagnosticRoute::Auto;
uint32_t fallbackCost = 0, lastFallbackBudget = 0, lastOpenBudget = 0, lastWriteBudget = 0;
unsigned fallbacks = 0, opens = 0, closes = 0;
size_t relayWriteLimit = SIZE_MAX;
bool abortWrite = false;
struct Channel { bool used = false, up = false; std::string sent, received; };
std::array<Channel, 4> channels;
void reset() {
  completeDns(); dnsWait = dnsFail = rejectDnsSchedule = false; dnsCalls = 0;
  nativeSuccess = true; nativeCost = 0; nativeWriteLimit = SIZE_MAX;
  nativeHosts.clear(); nativeBytes.clear(); ca = nullptr; insecure = false;
  enabled = ready = allowFallback = true; phone = false;
  route = DiagnosticRoute::Auto;
  fallbackCost = lastFallbackBudget = lastOpenBudget = lastWriteBudget = 0;
  fallbacks = opens = closes = 0; relayWriteLimit = SIZE_MAX; abortWrite = false;
  channels = {}; WiFi.state = WL_CONNECTED; Host::resetClock();
}
}
// Fronteiras de seleção/lifecycle e rádio, não uma cópia da política de rota.
// Estes testes cobrem o NetworkClient.cpp ESP32 e HttpClient.cpp reais. A
// implementação BLE/protocolo e a política ESP32 NetworkRoute não são simuladas
// como se fossem transporte real; o guard host tem uma suíte separada.
namespace NetworkRoute {
bool phoneSelected() { return RouteTest::phone; }
bool fallbackEnabled() { return RouteTest::enabled; }
DiagnosticRoute diagnosticRoute() { return RouteTest::route; }
void setDiagnosticRoute(DiagnosticRoute route) { RouteTest::route = route; }
void finishRetainingPhone(uint32_t) {}
void service() {}
bool fallbackToPhone(uint32_t budget) {
  using namespace RouteTest;
  ++fallbacks; lastFallbackBudget = budget;
  if (enabled && phone && ready) return true;
  if (!enabled || !allowFallback || !budget) return false;
  delay(min(fallbackCost, budget));
  if (fallbackCost >= budget) return false;
  return phone = ready;
}
}
namespace PhoneRelay {
int open(const char *, uint16_t, uint32_t budget) {
  using namespace RouteTest; ++opens; lastOpenBudget = budget;
  if (!RouteTest::ready || !budget) return -1;
  for (size_t i = 0; i < channels.size(); ++i) {
    if (!channels[i].used) { channels[i].used = channels[i].up = true; return static_cast<int>(i); }
  }
  return -1;
}
size_t write(int id, const uint8_t *data, size_t size, uint32_t budget) {
  using namespace RouteTest; lastWriteBudget = budget;
  auto &channel = channels.at(id);
  if (!channel.up || !budget) return 0;
  size_t accepted = min(size, relayWriteLimit);
  channel.sent.append(reinterpret_cast<const char *>(data), accepted);
  if (abortWrite) channel.up = false;
  return accepted;
}
int read(int id, uint8_t *data, size_t size) {
  auto &rx = RouteTest::channels.at(id).received;
  if (rx.empty()) return -1;
  size_t got = min(size, rx.size()); memcpy(data, rx.data(), got); rx.erase(0, got); return static_cast<int>(got);
}
int available(int id) { return static_cast<int>(RouteTest::channels.at(id).received.size()); }
int peek(int id) { const auto &rx = RouteTest::channels.at(id).received; return rx.empty() ? -1 : uint8_t(rx[0]); }
bool connected(int id) { return RouteTest::channels.at(id).up; }
void close(int id) { RouteTest::channels.at(id) = {}; ++RouteTest::closes; }
}

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)
using namespace RouteTest;

void direct_tls_keeps_policy_and_no_phone() {
  reset(); NetworkClient client;
  const char root[] = "test CA"; client.setCACert(root);
  CHECK(client.connect("direct.example", 443, 3000));
  CHECK(!client.isPhone() && nativeHosts.size() == 1 && opens == 0 && fallbacks == 0);
  CHECK(ca == root && timeout > 0 && uint32_t(timeout) + handshake <= 1500);
  CHECK(client.write(uint8_t('x')) == 1 && nativeBytes == "x");
}
void no_wifi_relay_has_no_local_dns() {
  reset(); WiFi.state = 0; NetworkClient client;
  CHECK(client.connect("relay.example", 443, 8000) && client.isPhone());
  CHECK(dnsCalls == 0 && nativeHosts.empty() && opens == 1);
  channels[0].received = "abc";
  CHECK(client.available() == 3 && client.peek() == 'a' && client.read() == 'a');
  uint8_t rx[2]; CHECK(client.read(rx, 2) == 2 && rx[0] == 'b' && rx[1] == 'c');
  CHECK(client.read() == -1); client.flush(); CHECK(client.connected());
}
void tls_failure_falls_back_before_any_body() {
  reset(); nativeSuccess = false; nativeCost = 600; fallbackCost = 100;
  NetworkClient client; CHECK(client.connect("same.example", 443, 3000));
  CHECK(client.isPhone() && opens == 1 && nativeBytes.empty());
  CHECK(lastOpenBudget == 2300 && dnsCalls == 1);
  CHECK(HttpClient::writeAll(client, String("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\ndata")));
  CHECK(channels[0].sent.find("data") != std::string::npos && nativeBytes.empty());
}
void dns_failure_and_late_callback_are_bounded() {
  reset(); dnsWait = true; NetworkClient client;
  CHECK(client.connect("slow.example", 443, 3000));
  CHECK(client.isPhone() && millis() == 500 && nativeHosts.empty());
  client.stop(); completeDns(); dnsWait = false; phone = false;
  CHECK(client.connectDirect("next.example", 443, 1000));
  CHECK(!client.isPhone() && dnsCalls == 2 && nativeHosts.back() == "next.example");
}
void incomplete_write_never_reopens_or_replays() {
  reset(); phone = true; NetworkClient client; CHECK(client.connect("relay.example", 443, 1000));
  unsigned beforeFallback = fallbacks; relayWriteLimit = 3; abortWrite = true;
  CHECK(!HttpClient::writeAll(client, String("MUTATION"), 50));
  CHECK(channels[0].sent == "MUT" && opens == 1 && fallbacks == beforeFallback);
  CHECK(!client.connected() && client.write(uint8_t('!')) == 0 && opens == 1);
}
void short_writes_continue_suffix_once() {
  reset(); phone = true; relayWriteLimit = 2; NetworkClient client;
  CHECK(client.connect("relay.example", 443, 1000));
  CHECK(HttpClient::writeAll(client, String("0123456789"), 100));
  CHECK(channels[0].sent == "0123456789" && opens == 1);
}
void selected_phone_bypasses_even_working_wifi() {
  reset(); phone = true; NetworkClient client;
  CHECK(client.connect("relay.example", 443, 1000));
  CHECK(client.isPhone() && dnsCalls == 0 && nativeHosts.empty());
}
void direct_socket_does_not_migrate_when_global_route_changes() {
  reset(); NetworkClient direct; CHECK(direct.connect("direct.example", 443, 2000));
  phone = true; NetworkClient relay; CHECK(relay.connect("relay.example", 443, 1000));
  CHECK(direct.write(uint8_t('d')) == 1 && relay.write(uint8_t('r')) == 1);
  CHECK(nativeBytes == "d" && channels[0].sent == "r");
}
void independent_channels_and_raii_close() {
  reset(); phone = true;
  { NetworkClient first; CHECK(first.connect("one.example", 443, 1000));
    { NetworkClient second; CHECK(second.connect("two.example", 443, 1000));
      CHECK(first.write(uint8_t('1')) && second.write(uint8_t('2')));
      CHECK(channels[0].sent == "1" && channels[1].sent == "2"); }
    CHECK(first.connected() && closes == 1); }
  CHECK(closes == 2);
}
void disabled_fallback_and_expired_deadline_do_not_open() {
  reset(); enabled = false; WiFi.state = 0; NetworkClient disabled;
  CHECK(!disabled.connect("relay.example", 443, 1000) && opens == 0);
  reset(); nativeSuccess = false; nativeCost = 1000; NetworkClient expired;
  CHECK(!expired.connect("relay.example", 443, 1000) && opens == 0 && millis() == 1000);
}
void timeout_seconds_and_write_milliseconds() {
  struct InspectableClient : NetworkClient {
    unsigned long tlsWriteTimeout() const { return sslclient->socket_timeout; }
  };
  reset(); phone = true; InspectableClient client;
  CHECK(client.connect("relay.example", 443, 1000));
  unsigned long previousTlsTimeout = client.tlsWriteTimeout();
  client.setTimeout(2); CHECK(client.write(uint8_t('x')) && lastWriteBudget == 2000);
  client.setWriteTimeoutMs(17); CHECK(client.write(uint8_t('y')) && lastWriteBudget == 17);
  CHECK(client.tlsWriteTimeout() == previousTlsTimeout);
}
void dns_slots_quarantine_old_callbacks_without_uaf() {
  reset(); enabled = false; dnsWait = true;
  for (int i = 0; i < 4; ++i) {
    NetworkClient client; CHECK(!client.connect("pending.example", 443, 900));
  }
  CHECK(pendingDns.size() == 4 && millis() == 1200);
  NetworkClient client; CHECK(!client.connect("fifth.example", 443, 900));
  CHECK(dnsCalls == 4 && millis() == 1200);
  completeDns(); dnsWait = false;
  CHECK(client.connect("recovered.example", 443, 900) && dnsCalls == 5);
}
void channel_exhaustion_fails_closed_without_native_escape() {
  reset(); phone = true; NetworkClient clients[5];
  for (int i = 0; i < 4; ++i) CHECK(clients[i].connect("relay.example", 443, 1000));
  CHECK(!clients[4].connect("relay.example", 443, 1000));
  CHECK(clients[4].isPhone() && !clients[4].connected() && clients[4].write(uint8_t('!')) == 0);
  CHECK(nativeHosts.empty() && dnsCalls == 0 && opens == 5);
}
void connect_deadline_survives_millis_wrap() {
  reset(); Host::resetClock(UINT32_MAX - 250); dnsWait = true;
  uint32_t started = millis(); NetworkClient client;
  CHECK(client.connect("slow.example", 443, 3000));
  CHECK(client.isPhone() && uint32_t(millis() - started) == 500 && lastOpenBudget == 2500);
  completeDns();
}
void native_incomplete_write_never_switches_to_phone() {
  reset(); NetworkClient client; CHECK(client.connect("direct.example", 443, 2000));
  CHECK(client.write(reinterpret_cast<const uint8_t *>("MUT"), 3) == 3);
  nativeWriteLimit = 0;
  CHECK(!HttpClient::writeAll(client, String("ATION"), 50));
  CHECK(nativeBytes == "MUT" && opens == 0 && fallbacks == 0 && !client.isPhone());
}
void wifi_only_does_not_fallback_after_native_failure() {
  reset(); route = DiagnosticRoute::WifiOnly; nativeSuccess = false;
  NetworkClient client; CHECK(!client.connect("wifi-only.example", 443, 1500));
  CHECK(nativeHosts.size() == 1 && fallbacks == 0 && opens == 0);
}
void phone_only_skips_native_even_when_wifi_is_healthy() {
  reset(); route = DiagnosticRoute::PhoneOnly; nativeSuccess = true;
  NetworkClient client; CHECK(client.connect("phone-only.example", 443, 1500));
  CHECK(nativeHosts.empty() && fallbacks == 2 && opens == 1);
}
int main() {
  void (*tests[])() = {direct_tls_keeps_policy_and_no_phone, no_wifi_relay_has_no_local_dns,
    tls_failure_falls_back_before_any_body, dns_failure_and_late_callback_are_bounded,
    incomplete_write_never_reopens_or_replays, short_writes_continue_suffix_once,
    selected_phone_bypasses_even_working_wifi, direct_socket_does_not_migrate_when_global_route_changes,
    independent_channels_and_raii_close, disabled_fallback_and_expired_deadline_do_not_open,
    timeout_seconds_and_write_milliseconds, dns_slots_quarantine_old_callbacks_without_uaf,
    channel_exhaustion_fails_closed_without_native_escape, connect_deadline_survives_millis_wrap,
    native_incomplete_write_never_switches_to_phone, wifi_only_does_not_fallback_after_native_failure,
    phone_only_skips_native_even_when_wifi_is_healthy};
  for (auto test : tests) test();
  std::printf("NetworkClient ESP32 real + SDK/PhoneRelay stubs: %zu/%zu OK\n", std::size(tests), std::size(tests));
}
