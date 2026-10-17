#include "../../src/assistant/live_protocol.h"
#include "../../src/assistant/ws_codec.h"
#include "../../src/assistant/tool_schema.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace Assistant;
using E = WsParser::Event;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
static std::vector<uint8_t> frame(uint8_t op, const std::string &s, bool fin = true) {
  std::vector<uint8_t> out = {uint8_t((fin ? 128 : 0) | op)};
  if (s.size() < 126) out.push_back(uint8_t(s.size()));
  else if (s.size() < 65536) { out.push_back(126); out.push_back(uint8_t(s.size() >> 8)); out.push_back(uint8_t(s.size())); }
  else { out.push_back(127); for (int i = 7; i >= 0; --i) out.push_back(uint8_t(uint64_t(s.size()) >> (i * 8))); }
  out.insert(out.end(), s.begin(), s.end()); return out;
}
static E feed(WsParser &p, const std::vector<uint8_t> &bytes) {
  E last = E::None;
  for (size_t i = 0; i < bytes.size(); ++i) {
    last = p.feed(bytes[i]);
    if (last == E::Error) break;
    if (i + 1 < bytes.size()) CHECK(last == E::None);
  }
  return last;
}
static void parserTests() {
  uint8_t buf[256];
  {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, frame(1, "Hello")) == E::Message);
    CHECK(p.size() == 5 && !memcmp(p.data(), "Hello", 5)); CHECK(p.finish());
  }
  {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, frame(1, "\xe2", false)) == E::None);
    CHECK(feed(p, frame(9, "ping")) == E::Ping); CHECK(p.size() == 4);
    CHECK(feed(p, frame(10, "pong")) == E::Pong);
    CHECK(feed(p, frame(0, "\x82\xac")) == E::Message); CHECK(p.size() == 3); CHECK(p.finish());
  }
  {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, frame(2, std::string(126, 'a'))) == E::Message);
    CHECK(p.size() == 126);
  }
  {
    std::vector<uint8_t> big(70001); WsParser p(big.data(), big.size());
    CHECK(feed(p, frame(2, std::string(70000, 'z'))) == E::Message); CHECK(p.size() == 70000);
  }
  for (const auto &bad : std::vector<std::vector<uint8_t>>{
      {0x81, 0x80}, {0xc1, 0}, {0x83, 0}, {0x89, 126}, {0x09, 0}, {0x80, 0},
      {0x81, 126, 0, 1}, {0x81, 127, 0x80, 0, 0, 0, 0, 0, 0, 0},
      {0x81, 127, 0, 0, 0, 0, 0, 0, 0xff, 0xff},
      {0x88, 1, 0}, {0x88, 2, 3, 237}, {0x88, 2, 0, 10}, {0x88, 4, 3, 232, 0xc0, 0x80},
      {0x81, 2, 0xc0, 0x80}, {0x81, 3, 0xed, 0xa0, 0x80}, {0x81, 4, 0xf4, 0x90, 0x80, 0x80}}) {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, bad) == E::Error);
  }
  {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, frame(1, "a", false)) == E::None);
    CHECK(feed(p, frame(2, "b")) == E::Error);
  }
  {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, frame(1, std::string(255, 'a'), false)) == E::None);
    CHECK(feed(p, frame(0, "b")) == E::Error);
  }
  for (size_t cut = 1; cut < 9; ++cut) {
    auto f = frame(1, "abcdefg"); f.resize(cut); WsParser p(buf, sizeof(buf));
    feed(p, f); CHECK(!p.finish()); CHECK(!strcmp(p.error(), "WS_TRUNCATED"));
  }
  {
    WsParser p(buf, sizeof(buf)); feed(p, frame(1, "a", false)); CHECK(!p.finish());
  }
  {
    WsParser p(buf, sizeof(buf)); CHECK(feed(p, frame(8, std::string("\x03\xe8", 2))) == E::Close);
  }
  const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  uint8_t bytes[] = {'H','e','l','l','o'}, header[14];
  CHECK(wsClientHeader(header, 1, 5, mask) == 6); CHECK(header[0] == 0x81 && header[1] == 0x85);
  wsMask(bytes, 5, mask); const uint8_t known[] = {0x7f,0x9f,0x4d,0x51,0x58};
  CHECK(!memcmp(bytes, known, 5)); wsMask(bytes, 5, mask); CHECK(!memcmp(bytes, "Hello", 5));
  CHECK(wsClientHeader(header, 1, 126, mask) == 8 && header[1] == 254 && header[3] == 126);
  CHECK(wsClientHeader(header, 1, 65536, mask) == 14 && header[1] == 255 && header[7] == 1);
  std::string valid = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
  CHECK(wsUpgradeValid(valid, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
  CHECK(!wsUpgradeValid(valid, "wrong"));
  CHECK(!wsUpgradeValid(valid.substr(0, valid.size() - 2), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
  valid.insert(valid.size() - 2, "Sec-WebSocket-Extensions: permessage-deflate\r\n");
  CHECK(!wsUpgradeValid(valid, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}
static Json json(const std::string &s) { return parseJson(s.data(), s.size()); }
static void jsonTests() {
  CHECK(!allowedModel("", true)); CHECK(!allowedModel("gemini-3.8-flash", true));
  CHECK(!allowedModel("gemini-3.8-live", false)); CHECK(!allowedModel("gemini-3.8-live-extended-thinking", true));
  CHECK(allowedModel("models/gemini-3.8-live", true)); CHECK(allowedModel("gemini-3.1-flash-live-preview", true));
  CHECK(setupJson("gemini-3.8-live", false, "America/Sao_Paulo", 0, true).empty());
  CHECK(setupJson("gemini-3.8-live", true, "bad\nzone", 0, true).empty());
  auto setup = json(setupJson("gemini-3.8-live", true, "America/Sao_Paulo", 0, true)); CHECK(bool(setup));
  const cJSON *s = field(setup.get(), "setup");
  CHECK(!strcmp(stringField(s, "model"), "models/gemini-3.8-live"));
  CHECK(trueField(field(field(s, "realtimeInputConfig"), "automaticActivityDetection"), "disabled"));
  CHECK(cJSON_IsObject(field(s, "inputAudioTranscription")) && cJSON_IsObject(field(s, "outputAudioTranscription")));
  CHECK(!strcmp(cJSON_GetArrayItem(field(field(s, "generationConfig"), "responseModalities"), 0)->valuestring, "AUDIO"));
  std::string text = printJson(field(s, "systemInstruction"));
  CHECK(text.find("relógio inválido") != std::string::npos); CHECK(text.find("use web_search") != std::string::npos);
  CHECK(text.find("TOOL_DISABLED") != std::string::npos);
  setup = json(setupJson("gemini-3.1-flash-live-preview", true, "America/Sao_Paulo", 1792108800, true));
  s = field(setup.get(), "setup"); text = printJson(field(s, "systemInstruction"));
  CHECK(text.find("2026-") != std::string::npos && text.find("America/Sao_Paulo") != std::string::npos);
  CHECK(printJson(field(s, "tools")).find("BLOCKING") == std::string::npos);
  auto start = json(activityJson(true)), end = json(activityJson(false));
  CHECK(cJSON_IsObject(field(field(start.get(), "realtimeInput"), "activityStart")));
  CHECK(cJSON_IsObject(field(field(end.get(), "realtimeInput"), "activityEnd")));
  uint8_t pcm[] = {0, 1, 0xff, 0x7f}; auto audio = json(audioJson(pcm, sizeof(pcm)));
  CHECK(!strcmp(stringField(field(field(audio.get(), "realtimeInput"), "audio"), "mimeType"), "audio/pcm;rate=16000"));
  CHECK(audioJson(pcm, 3).empty());
  std::vector<uint8_t> decoded;
  CHECK(unbase64(base64(pcm, 4).c_str(), 8, decoded, 4)); CHECK(decoded == std::vector<uint8_t>(pcm, pcm + 4));
  for (const auto &bad : {"A", "====", "AA=A", "AB==", "AAB=", "AA\n=", "@@@@"})
    CHECK(!unbase64(bad, strlen(bad), decoded, 100));
  CHECK(!unbase64("AAAA", 4, decoded, 2));
  for (size_t n = 0; n < 2049; ++n) {
    std::vector<uint8_t> in(n); for (size_t i = 0; i < n; ++i) in[i] = uint8_t(i * 17);
    std::string b64 = base64(in.data(), n); CHECK(unbase64(b64.data(), b64.size(), decoded, n)); CHECK(decoded == in);
  }
  CHECK(!json("{\"a\":1,\"a\":2}")); CHECK(!json("{\"n\":{\"a\":1,\"a\":2}}"));
  CHECK(!json("{\"id\":\"a\\u0000suffix\"}")); CHECK(!json("{}suffix")); CHECK(!json("{\"a\":\"\xc0\x80\"}"));
  CHECK(bool(json(R"({"text":"literal \\u0000 allowed"})")));
  CHECK(!json(std::string(20, '[') + "{}" + std::string(20, ']')));
  std::string result = toolError("OUTCOME_UNKNOWN", "Não confirmado.", true);
  auto tr = json(toolResponseJson("original:id/1", "vault_append", result)); CHECK(bool(tr));
  const cJSON *entry = cJSON_GetArrayItem(field(field(tr.get(), "toolResponse"), "functionResponses"), 0);
  CHECK(!strcmp(stringField(entry, "id"), "original:id/1"));
  CHECK(!trueField(field(entry, "response"), "ok"));
  CHECK(trueField(field(field(entry, "response"), "error"), "uncertain"));
  const std::string token(40, 's'), id = gatewayId(0x12345678aabbccddULL, 7);
  CHECK(id == "a12345678aabbccdd_7"); CHECK(id.size() <= 80);
  std::string payload = gatewayRequest(token, id, "vault_append", "{\"path\":\"a.md\",\"text\":\"x\"}");
  CHECK(!payload.empty() && payload == gatewayRequest(token, id, "vault_append", "{\"path\":\"a.md\",\"text\":\"x\"}"));
  CHECK(gatewayRequest(token, "bad:id", "vault_append", "{}").empty());
  CHECK(!gatewayRequest(token, id, "web_search", "{\"query\":\"pesquisa\"}").empty());
  CHECK(gatewayRequest(token, id, "unknown_tool", "{}").empty());
  CHECK(gatewayRequest("short", id, "vault_read", "{}").empty());
  const std::string safe = redact("secret-API " + token + " wss://host/?key=secret-API", "secret-API", token);
  CHECK(safe.find(token) == std::string::npos && safe.find("secret-API") == std::string::npos && safe.find("wss://") == std::string::npos);
  CHECK(redact("Authorization: Bearer unknown-secret fim", "", "").find("unknown-secret") == std::string::npos);
  auto schema = json(kToolSchema); CHECK(cJSON_GetArraySize(field(schema.get(), "functionDeclarations")) == 8);
}
static void redirectTests() {
  std::string path;
  CHECK(gatewayOrigin("https://script.google.com/macros/s/AbC_123-xyz/exec", path));
  CHECK(path == "/macros/s/AbC_123-xyz/exec");
  for (const auto &url : {"http://script.google.com/macros/s/id/exec", "https://script.google.com.evil/macros/s/id/exec",
      "https://script.google.com:443/macros/s/id/exec", "https://user@script.google.com/macros/s/id/exec",
      "https://script.google.com/macros/s/../exec", "https://script.google.com/macros/s/id/exec?token=bad",
      "https://script.google.com/macros/s/id/exec\r\nX: y", "https://script.google.com/macros/s//exec"}) CHECK(!gatewayOrigin(url, path));
  CHECK(gatewayRedirect("https://script.googleusercontent.com/macros/echo?x=y&z=1", path));
  CHECK(path == "/macros/echo?x=y&z=1");
  for (const auto &url : {"http://script.googleusercontent.com/a", "https://script.googleusercontent.com.evil/a",
      "https://script.googleusercontent.com@evil/a", "//script.googleusercontent.com/a", "/relative",
      "https://script.googleusercontent.com:443/a", "https://script.googleusercontent.com/a#fragment",
      "https://script.googleusercontent.com/a\nBad: header", "https://script.googleusercontent.com\\@evil/a"}) CHECK(!gatewayRedirect(url, path));
}
int main() {
  parserTests(); jsonTests(); redirectTests();
  std::cout << "Assistant protocolo: " << checks << " verificações OK\n";
}
