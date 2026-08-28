#pragma once
#include <Arduino.h>
#include "settings.h"

// Envia um WAV (LittleFS) para uma API de transcricao (Gemini / OpenAI /
// Groq) e gera resumos estruturados em Markdown via LLM.
class SttClient {
public:
  static constexpr size_t kMaxTextLen = 4096;
  bool transcribe(const Settings &cfg, const char *wavPath, char *outText, size_t outLen);
  bool generateSummary(const Settings &cfg, const char *transcriptText, char *outMarkdown, size_t outLen);
};
