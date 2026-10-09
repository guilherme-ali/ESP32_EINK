#pragma once

#include <Arduino.h>
#include <FS.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <initializer_list>
#include "settings.h"

// Transporte privado à integração STT. Não altera HttpClient nem os hooks
// globais do cJSON. Chamadas síncronas, serializadas pelo fluxo do aplicativo.
namespace SttNet {
constexpr size_t kMaxBody = 128 * 1024;
constexpr size_t kMaxText = 32768;
constexpr unsigned kMaxRetries = 2; // uma chamada inicial + até duas repetições
constexpr uint32_t kReadTimeoutMs = 60000;

struct Diagnostics {
  String error;
  String model; // modelo efetivamente chamado; versão da API tem preferência
  int status = 0;
  void reset() { error = ""; model = ""; status = 0; }
};

struct Response {
  int status = 0;
  uint32_t retryAfterSec = 0;
  String uploadUrl;
  String uploadStatus;
  String error;
  char *body = nullptr; // heap_caps: PSRAM primeiro, heap interno como fallback
  size_t length = 0;
  size_t capacity = 0;
  bool complete = false;
  Response() = default;
  ~Response();
  Response(const Response &) = delete;
  Response &operator=(const Response &) = delete;
  void reset();
  bool append(const uint8_t *data, size_t count);
};

class JsonDocument {
public:
  explicit JsonDocument(const Response &response);
  ~JsonDocument() { cJSON_Delete(root); }
  JsonDocument(const JsonDocument &) = delete;
  JsonDocument &operator=(const JsonDocument &) = delete;
  cJSON *root = nullptr;
};

cJSON *item(cJSON *parent, const char *key);
const char *text(cJSON *parent, const char *key);
bool validUtf8(const char *data, size_t length);
bool quote(const char *value, String &out); // escaping via cJSON
bool join(String &out, std::initializer_list<const char *> pieces);
bool appendText(const char *value, char *out, size_t outLen, size_t &used);
bool hasText(const char *value);
bool safeModel(const String &model);
String normalizedModel(const char *value);
bool safeHeader(const char *value);
bool beginRequest(WiFiClientSecure &client, const String &host, uint16_t port,
                  const char *method, const String &path, const char *key,
                  bool google, const char *contentType, size_t length,
                  const String &extraHeaders, Response &response);
bool write(WiFiClientSecure &client, const uint8_t *data, size_t length,
           uint32_t timeoutMs = 60000);
bool write(WiFiClientSecure &client, const String &value);
bool writeFile(WiFiClientSecure &client, File &file, bool base64 = false);
bool readResponse(WiFiClientSecure &client, Response &response,
                   uint32_t timeoutMs = kReadTimeoutMs);
bool finishRequest(WiFiClientSecure &client, Response &response, bool bodySent,
                   uint32_t timeoutMs = kReadTimeoutMs);
bool requestJson(const String &host, uint16_t port, const char *method,
                 const String &path, const char *key, bool google,
                 const String &body, Response &response,
                 const String &extraHeaders = "", uint32_t timeoutMs = kReadTimeoutMs);
void recordFailure(const Response &response, Diagnostics &diagnostics);
bool retryable(const Response &response);
uint32_t retryHint(const Response &response);
void backoff(unsigned retry, uint32_t seconds);
bool extractGemini(const Response &response, bool interaction, char *out,
                   size_t outLen, Diagnostics &diagnostics, uint32_t outputLimit = 0);
bool extractCompatible(const Response &response, bool summary, char *out,
                       size_t outLen, Diagnostics &diagnostics);
extern const char kSummaryPrompt[];
extern const char kTranscribePrompt[];
} // namespace SttNet

class GeminiClient {
public:
  bool transcribe(const Settings &cfg, File &wav, char *out, size_t outLen,
                  SttNet::Diagnostics &diagnostics);
  bool generateSummary(const Settings &cfg, const char *transcript, char *out,
                       size_t outLen, SttNet::Diagnostics &diagnostics);
};
