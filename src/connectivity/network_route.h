#pragma once
#include <stdint.h>

struct Settings;
class SettingsStore;
class WifiManager;

namespace NetworkRoute {
#if defined(ARDUINO_ARCH_ESP32)
// Lifecycle configure/connect/finish no coordenador, fora de callbacks.
// configure: apenas sem sessão/clientes/tasks concorrentes. Copia só BLE/PIN.
void configure(const Settings &settings);
// Prazo TOTAL: ~5 s associação, ~2 s probe HTTPS/204, restante BLE.
// scanNetworks e init NimBLE são síncronos no SDK; seu tempo não é preemptível.
bool connect(WifiManager &wifi, SettingsStore &settings, uint32_t timeoutMs = 15000);
bool online(); // disponibilidade da rota selecionada; não é probe por chamada
bool fallbackToPhone(uint32_t timeoutMs = 8000);
bool phoneSelected();
const char *label(); // literal estático: "Wi-Fi", "Telefone BLE", "offline"
// Fechar TODOS os clientes e juntar workers antes de finish; stop retém stack.
void finish();
// Só recupera se caiu; telefone válido nunca provoca scan/reassociação Wi-Fi.
bool recover(uint32_t timeoutMs = 8000);
// Extra para reservar orçamento nativo antes de fallback; leitura thread-safe.
bool fallbackEnabled();
#else
// Host não depende do BLE nem muda os mocks atuais do transporte. Quando há
// stub WiFi.h, online/recover observam seu status; sem stub, rede fica disponível
// para os testes HTTP baseados exclusivamente em WiFiClientSecure.
bool online();
inline void configure(const Settings &) {}
inline bool connect(WifiManager &, SettingsStore &, uint32_t = 15000) { return online(); }
inline bool fallbackToPhone(uint32_t = 8000) { return false; }
inline bool phoneSelected() { return false; }
inline const char *label() { return online() ? "Wi-Fi" : "offline"; }
inline void finish() {}
inline bool recover(uint32_t = 8000) { return online(); }
inline bool fallbackEnabled() { return false; }
#endif
}

#if !defined(ARDUINO_ARCH_ESP32)
#if __has_include(<WiFi.h>)
#include <WiFi.h>
inline bool NetworkRoute::online() { return WiFi.status() == WL_CONNECTED; }
#else
inline bool NetworkRoute::online() { return true; }
#endif
#endif
