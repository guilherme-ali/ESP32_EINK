#pragma once
#include <Arduino.h>
#include "settings.h"
#include "gemini_client.h"

// O chamador fornece os buffers (preferencialmente em PSRAM). Falhas sempre
// deixam o destino vazio; texto parcial/truncado nunca representa sucesso.
class SttClient {
public:
  static constexpr size_t kMaxTextLen = 128 * 1024; // PSRAM; capacidade incluindo NUL
  bool transcribe(const Settings &cfg, const char *wavPath, char *outText, size_t outLen);
  bool generateSummary(const Settings &cfg, const char *transcriptText, char *outMarkdown, size_t outLen);
  static void beginBatch();
  static void cleanupPending(const Settings &cfg);
  static void endSession();
  const String &lastError() const { return diagnostics_.error; }
  int lastStatusCode() const { return diagnostics_.status; }
  const String &lastModel() const { return diagnostics_.model; }

private:
  SttNet::Diagnostics diagnostics_;
};
