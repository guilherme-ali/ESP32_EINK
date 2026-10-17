#pragma once

#include <WiFiClientSecure.h>

#if defined(ARDUINO_ARCH_ESP32)
// Um socket, um transporte até stop(). HTTP/WSS continuam nos clientes atuais;
// no telefone os bytes passam por BLE autenticado e pelo SSLSocket Android.
// Não compartilhar uma instância entre threads e nunca copiar canais.
class NetworkClient : public WiFiClientSecure {
public:
  NetworkClient() = default;
  ~NetworkClient() override;
  NetworkClient(const NetworkClient &) = delete;
  NetworkClient &operator=(const NetworkClient &) = delete;
  NetworkClient(NetworkClient &&) = delete;
  NetworkClient &operator=(NetworkClient &&) = delete;

  int connect(const char *host, uint16_t port) override;
  int connect(const char *host, uint16_t port, int32_t timeoutMs) override;
  int available() override;
  int read() override;
  int read(uint8_t *buffer, size_t size) override;
  int peek() override;
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *buffer, size_t size) override;
  uint8_t connected() override;
  void stop() override;
  void flush() override; // como o core TLS: não descarta nem espera RX/TX
  int setTimeout(uint32_t seconds) override; // mesma unidade/retorno do core 2.0.17

  // Extras: probe exclusivamente nativo, prazo de write em ms e rota DO SOCKET
  // (a seleção global pode mudar enquanto um socket direto ainda está vivo).
  // connectDirect não seleciona BLE nem envia dados de aplicação.
  int connectDirect(const char *host, uint16_t port, uint32_t timeoutMs);
  void setWriteTimeoutMs(uint32_t milliseconds);
  bool isPhone() const { return phone_; }

private:
  int connectPhone(const char *host, uint16_t port, uint32_t timeoutMs);
  int channel_ = -1;
  bool phone_ = false;
  uint32_t writeTimeoutMs_ = 20000;
};
#else
// Mantém os mocks/assinaturas dos testes host existentes sem link com PhoneRelay.
using NetworkClient = WiFiClientSecure;
#endif
