#pragma once
#include <Arduino.h>
#include "settings.h"

// Envia um WAV (LittleFS) para uma API de transcricao compativel com o
// formato OpenAI (multipart/form-data, campo "file" + "model", resposta
// JSON {"text": "..."}) - cobre Groq, OpenAI e qualquer servidor local
// que implemente esse mesmo contrato, daí o endpoint ser configuravel.
class SttClient {
public:
  // Retorna true e preenche outText se a transcricao funcionar.
  // outText deve ter espaco suficiente (ver kMaxTextLen).
  static constexpr size_t kMaxTextLen = 2048;
  bool transcribe(const Settings &cfg, const char *wavPath, char *outText, size_t outLen);
};
