#pragma once

#include <cstddef>
#include <cstdint>

namespace PhoneRelay {

// Lifecycle (begin/stop/shutdown) chamado serialmente pelo coordenador,
// fora de callbacks. PIN numérico 0..999999, renderizado como seis dígitos
// pelo portal (inclusive zeros à esquerda). Este módulo não persiste o PIN.
bool begin(uint32_t passkey);
bool waitReady(uint32_t timeoutMs);
bool ready();
bool active(); // sessão solicitada, inclusive enquanto anuncia/pareia
void stop();

int open(const char* host, uint16_t port, uint32_t timeoutMs);
// Retorna bytes aceitos na fila, não confirmação TLS/HTTP. Falha/timeout aborta
// o canal e fica visível em connected/lastError. Nunca há replay automático.
size_t write(int channel, const uint8_t* data, size_t size, uint32_t timeoutMs);
// Não bloqueantes: read/peek retornam -1 se não houver bytes; available >= 0.
int read(int channel, uint8_t* data, size_t size);
int available(int channel);
int peek(int channel);
bool connected(int channel);
void close(int channel);
const char* lastError(); // literal estático; nunca inclui host/PIN/payload remoto
struct Diagnostics {
    bool linked, authenticated, subscribed;
    uint16_t mtu;
    int securityResult, bondResult, disconnectReason, authenticationFlags;
    uint32_t passkeyRequests;
    uint16_t interval1250us, latency, supervisionTimeout10ms;
    uint16_t maxTxOctets, maxRxOctets, maxTxTime, maxRxTime;
    uint8_t txPhy, rxPhy;
};
Diagnostics diagnostics(); // somente metadados; nunca chaves/PIN/payloads

// Opcional: após clientes fechados, para worker e libera objetos/stack NimBLE.
// É terminal (begin recusado). Timeout mantém objetos válidos; permite retry.
// stop() normalmente retém a stack para reutilização segura e sem reallocações.
// timeoutMs limita as esperas próprias; init/deinit internos são síncronos NimBLE.
bool shutdown(uint32_t timeoutMs = 2000);

} // namespace PhoneRelay
