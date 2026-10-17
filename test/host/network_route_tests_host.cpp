#include "../../src/connectivity/network_route.h"
#include "../../src/net/network_client.h"
#include <type_traits>
#include <cstdio>

// Compilar sem ARDUINO_ARCH_ESP32 e com o stub WiFi.h dedicado. Não precisa
// linkar network_route.cpp, network_client.cpp ou PhoneRelay no runner existente.
static_assert(std::is_same<NetworkClient, WiFiClientSecure>::value, "alias host mudou");
int main() {
  WiFi.state = WL_CONNECTED;
  if (!NetworkRoute::online() || !NetworkRoute::recover() || NetworkRoute::phoneSelected()) return 1;
  WiFi.state = 0;
  if (NetworkRoute::online() || NetworkRoute::recover() || NetworkRoute::fallbackToPhone()) return 2;
  if (std::string(NetworkRoute::label()) != "offline") return 3;
  NetworkRoute::finish();
  std::puts("NetworkRoute host guard: 3/3 OK (alias + WiFi status; sem BLE)");
}
