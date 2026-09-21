#pragma once

#include <Arduino.h>

// Le valores de respostas JSON pequenas sem fazer busca textual. path usa
// pontos e indices de array, por exemplo:
// candidates.0.content.parts.0.text
bool jsonGetString(const String &body, const char *path, char *out, size_t outLen);
bool jsonGetLong(const String &body, const char *path, long &out);
