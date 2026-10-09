#pragma once
#include <stddef.h>
#include <stdint.h>

// Um unico worker de rede publica eventos; callbacks nunca desenham no e-paper.
namespace SyncTelemetry {
enum class Phase : uint8_t {
  Preparing, GeminiUpload, Transcribing, Markdown, DriveUpload, Verifying,
  QuotaWait, RetryWait, Done, Error
};
struct Update {
  Phase phase = Phase::Preparing;
  uint64_t current = 0;
  uint64_t total = 0;
  const char *detail = ""; // valido somente durante o callback
};
using Observer = void (*)(const Update &);
struct Counters {
  uint32_t httpRequests = 0;
  uint32_t tlsHandshakes = 0;
  uint64_t geminiBytes = 0;
  uint64_t driveBytes = 0;
  uint32_t quotaWaitMs = 0;
  uint32_t retryWaitMs = 0;
  uint32_t preparingMs = 0;
  uint32_t geminiUploadMs = 0;
  uint32_t transcribeMs = 0;
  uint32_t markdownMs = 0;
  uint32_t driveUploadMs = 0;
  uint32_t verifyingMs = 0;
};
void reset(Observer observer = nullptr);
void setObserver(Observer observer);
void phase(Phase next, const char *detail = "");
void transfer(Phase next, uint64_t current, uint64_t total, const char *detail = "");
void httpRequest(const char *host = nullptr);
void tlsHandshake(const char *host = nullptr);
void sentBytes(Phase destination, size_t count);
// A espera ocorre somente no worker; UI e botoes continuam sendo atendidos.
void wait(uint32_t milliseconds, bool quota, const char *detail = "");
Counters counters();
}
