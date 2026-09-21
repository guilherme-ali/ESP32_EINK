#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <FS.h>

// Resposta HTTP limitada e totalmente consumida. O corpo fica apenas para
// respostas pequenas (JSON de API); uploads de arquivos usam o mesmo
// transporte, mas transmitem o corpo diretamente do LittleFS.
struct HttpResponse {
  int statusCode = 0;
  bool headersComplete = false;
  bool bodyComplete = false;
  bool bodyTruncated = false;
  bool chunked = false;
  bool hasContentLength = false;
  size_t contentLength = 0;
  uint32_t retryAfterSec = 0;
  String location;
  String range;
  String body;
};

class HttpClient {
public:
  static constexpr uint32_t kConnectTimeoutMs = 15000;
  static constexpr uint32_t kResponseTimeoutMs = 20000;
  static constexpr size_t kMaxResponseBody = 12288;

  // WiFiClientSecure::write() pode aceitar menos bytes que os pedidos.
  // Estas funções repetem até todo o buffer ser enviado ou o prazo acabar.
  static bool writeAll(WiFiClientSecure &client, const uint8_t *data, size_t len,
                       uint32_t timeoutMs = kResponseTimeoutMs);
  static bool writeAll(WiFiClientSecure &client, const String &data,
                       uint32_t timeoutMs = kResponseTimeoutMs);
  static bool writeFileChunk(WiFiClientSecure &client, File &file, size_t offset,
                              size_t length, uint32_t timeoutMs = kResponseTimeoutMs);

  // Le status, cabecalhos e corpo com prazo absoluto por chamada. Nunca
  // espera indefinidamente por um servidor que ficou conectado sem responder.
  static bool readResponse(WiFiClientSecure &client, HttpResponse &out,
                           size_t maxBody = kMaxResponseBody,
                           uint32_t timeoutMs = kResponseTimeoutMs);

  static bool parseHttpsUrl(const String &url, String &host, uint16_t &port,
                            String &path);
  static bool isRetryableStatus(int statusCode, const String &body);
  static uint32_t retryDelayMs(int attempt, uint32_t retryAfterSec);
};
