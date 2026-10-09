#pragma once
#include <string>

namespace Fixtures {
inline const char flash[] = R"({"modelVersion":"gemini-test-version","candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"secreto","thought":true},{"text":"Olá "},{"inlineData":{"data":"ignored"}},{"text":"mundo"}]}}]})";
inline const char maxTokens[] = R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"parcial"}]}}]})";
inline const char interaction[] = R"({"status":"completed","model":"models/gemini-test-version","outputs":[{"type":"text","thought":true,"text":"secreto"},{"type":"audio","text":"ignorado"},{"type":"text","text":"Olá "},{"type":"text","text":"mundo"}]})";
inline const char emptyInteraction[] = R"({"status":"completed","outputs":[]})";
inline const char interactionSteps[] = R"({"status":"completed","model":"gemini-3.5-transcribe","steps":[{"type":"thought","content":[{"type":"text","text":"ignorado"}]},{"type":"model_output","content":[{"type":"text","text":"Olá "},{"type":"text","text":"mundo"}]}]})";
inline const char compatibleSummary[] = R"({"model":"gpt-test","choices":[{"finish_reason":"stop","message":{"content":[{"type":"text","thought":true,"text":"segredo"},{"type":"text","text":"# Título\n"},{"type":"image","text":"ignorado"},{"type":"text","text":"- fato"}]}}]})";
inline std::string http(const std::string &body, int status = 200,
                        const std::string &headers = "") {
  return "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
}
inline std::string wav() {
  std::string data(60, '\0');
  data.replace(0, 4, "RIFF"); data[4] = 52; data.replace(8, 4, "WAVE");
  data.replace(12, 4, "fmt "); data[16] = 16; data[20] = 1; data[22] = 1;
  data[24] = '\x80'; data[25] = '\x3e'; data[28] = '\x00'; data[29] = '\x7d';
  data[32] = 2; data[34] = 16; data.replace(36, 4, "data"); data[40] = 16;
  return data;
}
}
