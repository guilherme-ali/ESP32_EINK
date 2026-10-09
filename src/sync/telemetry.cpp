#include "telemetry.h"
#include <Arduino.h>

namespace SyncTelemetry {
namespace {
Observer callback = nullptr;
Counters values;
Phase active = Phase::Preparing;
uint32_t since = 0;
void accrue(uint32_t elapsed) {
  switch (active) {
    case Phase::Preparing: values.preparingMs += elapsed; break;
    case Phase::GeminiUpload: values.geminiUploadMs += elapsed; break;
    case Phase::Transcribing: values.transcribeMs += elapsed; break;
    case Phase::Markdown: values.markdownMs += elapsed; break;
    case Phase::DriveUpload: values.driveUploadMs += elapsed; break;
    case Phase::Verifying: values.verifyingMs += elapsed; break;
    case Phase::QuotaWait: values.quotaWaitMs += elapsed; break;
    case Phase::RetryWait: values.retryWaitMs += elapsed; break;
    default: break;
  }
}
}
void reset(Observer observer) {
  callback = observer; values = Counters(); active = Phase::Preparing; since = millis();
}
void setObserver(Observer observer) { callback = observer; }
void phase(Phase next, const char *detail) {
  uint32_t now = millis(); accrue(now - since); since = now; active = next;
  if (callback) { Update event; event.phase = next; event.detail = detail; callback(event); }
}
void transfer(Phase next, uint64_t current, uint64_t total, const char *detail) {
  if (next != active) { uint32_t now = millis(); accrue(now - since); since = now; active = next; }
  if (callback) { Update event; event.phase = next; event.current = current; event.total = total;
    event.detail = detail; callback(event); }
}
void httpRequest(const char *) { ++values.httpRequests; }
void tlsHandshake(const char *) { ++values.tlsHandshakes; }
void sentBytes(Phase destination, size_t count) {
  if (destination == Phase::GeminiUpload) values.geminiBytes += count;
  if (destination == Phase::DriveUpload) values.driveBytes += count;
}
void wait(uint32_t milliseconds, bool quota, const char *detail) {
  Phase previous = active;
  phase(quota ? Phase::QuotaWait : Phase::RetryWait, detail);
  delay(milliseconds);
  phase(previous);
}
Counters counters() {
  uint32_t now = millis(); accrue(now - since); since = now;
  return values;
}
}
