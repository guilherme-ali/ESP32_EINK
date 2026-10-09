#include "fixtures.h"
#include "http_client.h"
#include "json_utils.h"
#include "gemini_client.h"
#include "stt.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace NoteFiles { fs::FS &fs(); }

namespace {
struct Test { const char *name; std::function<void()> run; };
std::vector<Test> &tests() { static std::vector<Test> all; return all; }
struct Register {
  Register(const char *name, std::function<void()> run) { tests().push_back({name, run}); }
};
#define TEST(name) void name(); Register register_##name(#name, name); void name()
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
  std::string("linha ") + std::to_string(__LINE__) + ": " + #condition); } while (false)

WiFiClientSecure stream(const std::string &bytes, size_t fragment = SIZE_MAX,
                        uint32_t gapMs = 0, bool keepOpen = false) {
  WiFiClientSecure client;
  client.feed({bytes, fragment, gapMs, keepOpen, SIZE_MAX});
  return client;
}
void response(SttNet::Response &out, const std::string &body, bool complete = true, int status = 200) {
  CHECK(out.append(reinterpret_cast<const uint8_t *>(body.data()), body.size()));
  out.complete = complete; out.status = status;
}
SttNet::Diagnostics diagnostics() {
  SttNet::Diagnostics d; d.model = "requested-model"; return d;
}
Settings compatibleSettings() {
  Settings cfg;
  std::strcpy(cfg.sttEndpoint, "https://api.openai.com/v1/audio/transcriptions");
  std::strcpy(cfg.sttApiKey, "fake-key");
  return cfg;
}

TEST(http_fragmented_content_length) {
  for (size_t fragment : {size_t(1), size_t(2), size_t(7), size_t(64), SIZE_MAX}) {
    Host::resetClock();
    auto client = stream(Fixtures::http("Olá mundo"), fragment, 1, true);
    HttpResponse r;
    CHECK(HttpClient::readResponse(client, r, 100, 1000));
    CHECK(r.statusCode == 200 && r.headersComplete && r.bodyComplete);
    CHECK(!r.bodyTruncated && !r.chunked && r.hasContentLength);
    CHECK(r.body == "Olá mundo");
    CHECK(client.consumed() == Fixtures::http("Olá mundo").size());
  }
}
TEST(interactions_current_steps_contract) {
  SttNet::Response r; response(r, Fixtures::interactionSteps);
  char output[128]; auto d = diagnostics();
  CHECK(SttNet::extractGemini(r, true, output, sizeof(output), d));
  CHECK(std::string(output) == "Olá mundo");
  CHECK(d.model == "gemini-3.5-transcribe");
}
TEST(early_http_error_survives_interrupted_request_body) {
  auto client = stream(Fixtures::http(R"({"error":{"code":429}})", 429));
  SttNet::Response r;
  CHECK(SttNet::finishRequest(client, r, false));
  CHECK(r.status == 429 && r.complete && r.error.isEmpty());
}
TEST(interrupted_body_cannot_be_confirmed_by_success_response) {
  auto client = stream(Fixtures::http("{}", 200));
  SttNet::Response r;
  CHECK(!SttNet::finishRequest(client, r, false));
  CHECK(r.error.length());
}
TEST(audio_upload_reads_early_error_before_sending_more_bytes) {
  auto client = stream(Fixtures::http(R"({"error":{"code":403}})", 403));
  File audio(Fixtures::wav());
  CHECK(!SttNet::writeFile(client, audio, true));
  CHECK(client.sent.empty());
  SttNet::Response r;
  CHECK(SttNet::finishRequest(client, r, false));
  CHECK(r.status == 403 && r.complete);
}
TEST(http_chunked_extensions_and_trailers) {
  for (size_t fragment : {size_t(1), size_t(3), SIZE_MAX}) {
    auto client = stream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                         "3;extension=yes\r\nabc\r\n2\r\nde\r\n0\r\nX-Trailer: yes\r\n\r\n", fragment, 1, true);
    HttpResponse r;
    CHECK(HttpClient::readResponse(client, r, 100, 1000));
    CHECK(r.headersComplete && r.bodyComplete && r.chunked && r.body == "abcde");
  }
}
TEST(http_truncated_header) {
  for (const std::string wire : {"HTTP/1.1 200", "HTTP/1.1 200 OK\r\nContent-Length: 4",
                                "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"}) {
    auto client = stream(wire, 1, 1);
    HttpResponse r;
    CHECK(!HttpClient::readResponse(client, r, 100, 1000));
    CHECK(!r.headersComplete && !r.bodyComplete && client.stopped());
  }
}
TEST(http_truncated_content_length_body) {
  auto client = stream("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc", 1, 1);
  HttpResponse r;
  CHECK(!HttpClient::readResponse(client, r, 100, 1000));
  CHECK(r.headersComplete && !r.bodyComplete && r.body == "abc" && client.stopped());
}
TEST(http_maxbody_content_length_consumes_remainder) {
  auto client = stream(Fixtures::http("abcdef"), 2, 1, true);
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 3, 1000));
  CHECK(r.bodyComplete && r.bodyTruncated && r.body == "abc");
  CHECK(client.consumed() == Fixtures::http("abcdef").size());
}
TEST(http_maxbody_chunked_consumes_trailers) {
  const std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                           "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n";
  auto client = stream(wire, 2, 1, true);
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 3, 1000));
  CHECK(r.bodyComplete && r.bodyTruncated && r.body == "abc");
  CHECK(client.consumed() == wire.size());
}
TEST(http_zero_maxbody) {
  auto client = stream(Fixtures::http("abc"));
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 0, 100));
  CHECK(r.bodyComplete && r.bodyTruncated && r.body.isEmpty());
}
TEST(http_308_without_range) {
  auto client = stream(Fixtures::http("", 308), 1, 1, true);
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 100, 1000));
  CHECK(r.statusCode == 308 && r.range.isEmpty() && r.headersComplete && r.bodyComplete);
}
TEST(http_308_preserves_range) {
  auto client = stream(Fixtures::http("", 308, "Range: bytes=0-255\r\n"));
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 100, 1000));
  CHECK(r.statusCode == 308 && r.range == "bytes=0-255");
}
TEST(http_unterminated_chunk_size_body_and_trailer) {
  for (const std::string suffix : {"3", "3\r\nab", "3\r\nabc", "3\r\nabc\r\n0\r\n",
                                  "0\r\nX-Trailer: unfinished"}) {
    auto client = stream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + suffix, 1, 1);
    HttpResponse r;
    CHECK(!HttpClient::readResponse(client, r, 100, 1000));
    CHECK(r.headersComplete && !r.bodyComplete && client.stopped());
  }
}
TEST(http_chunk_data_requires_empty_crlf_separator) {
  // Malformed stream: the line after chunk data is not the required empty CRLF.
  auto client = stream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "1\r\naINVALID\r\n0\r\n\r\n", 1, 1);
  HttpResponse r;
  CHECK(!HttpClient::readResponse(client, r, 100, 1000));
  CHECK(!r.bodyComplete);
}
TEST(http_204_and_retry_after_over_60) {
  auto client = stream("HTTP/1.1 204 No Content\r\nRetry-After: 120\r\n\r\n", 1, 1, true);
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 100, 1000));
  CHECK(r.statusCode == 204 && r.bodyComplete && r.body.isEmpty() && r.retryAfterSec == 120);
  CHECK(HttpClient::retryDelayMs(0, r.retryAfterSec) == UINT32_MAX);
}
TEST(http_retry_delay_bounds) {
  CHECK(HttpClient::retryDelayMs(5, 60) == 60000);
  CHECK(HttpClient::retryDelayMs(0, 61) == UINT32_MAX);
  CHECK(HttpClient::retryDelayMs(0, UINT32_MAX) == UINT32_MAX);
  for (int attempt : {-9, 0, 1, 5, 90}) {
    uint32_t base = 1000U << constrain(attempt, 0, 5);
    uint32_t got = HttpClient::retryDelayMs(attempt, 0);
    CHECK(got >= base && got <= base + 250);
  }
}
TEST(http_timeout_and_millis_wrap) {
  Host::resetClock(UINT32_MAX - 5);
  auto client = stream("", 1, 1, true);
  HttpResponse r;
  CHECK(!HttpClient::readResponse(client, r, 100, 20));
  CHECK(client.stopped() && millis() == 14);
}
TEST(http_close_delimited_body) {
  auto client = stream("HTTP/1.0 200 OK\r\n\r\nabcde", 1, 1);
  HttpResponse r;
  CHECK(HttpClient::readResponse(client, r, 3, 1000));
  CHECK(r.bodyComplete && r.bodyTruncated && r.body == "abc");
}
TEST(http_write_short_and_file_offset) {
  WiFiClientSecure client;
  client.feed({"", SIZE_MAX, 0, true, 2});
  CHECK(HttpClient::writeAll(client, String("abcdef"), 100));
  CHECK(client.sent == "abcdef");
  File file("0123456789", 2);
  CHECK(HttpClient::writeFileChunk(client, file, 3, 4, 100));
  CHECK(client.sent == "abcdef3456");
  CHECK(!HttpClient::writeFileChunk(client, file, 9, 4, 100));
}
TEST(http_write_no_progress_timeout) {
  WiFiClientSecure client;
  client.feed({"", SIZE_MAX, 0, true, 0});
  CHECK(!HttpClient::writeAll(client, String("abc"), 15));
  CHECK(millis() == 15);
}

TEST(json_real_cjson_nested_and_unicode) {
  char out[32]; long n = 0;
  String body = R"({"values":[{"text":"Ol\u00e1 \"mundo\"","number":42}]})";
  CHECK(jsonGetString(body, "values.0.text", out, sizeof(out)));
  CHECK(std::string(out) == "Olá \"mundo\"");
  CHECK(jsonGetLong(body, "values.0.number", n) && n == 42);
}
TEST(json_long_string_fails_without_partial_copy) {
  std::string text(4096, 'a');
  char out[32]; std::memset(out, 'Z', sizeof(out));
  CHECK(!jsonGetString(String("{\"text\":\"" + text + "\"}"), "text", out, sizeof(out)));
  CHECK(out[0] == '\0' && out[1] == 'Z' && out[31] == 'Z');
}
TEST(json_string_buffer_boundary_and_missing) {
  char out[4];
  CHECK(jsonGetString(R"({"text":"abc"})", "text", out, sizeof(out)));
  CHECK(std::string(out) == "abc");
  CHECK(!jsonGetString(R"({"text":"abcd"})", "text", out, sizeof(out)) && out[0] == '\0');
  for (const char *body : {"{", "{}", "{\"text\":42}", "{\"text\":\"\"}"})
    CHECK(!jsonGetString(body, "text", out, sizeof(out)) && out[0] == '\0');
  CHECK(!jsonGetString("{}", "text", nullptr, 4));
  CHECK(!jsonGetString("{}", "text", out, 0));
}

TEST(gemini_flash_joins_nonthought_parts) {
  SttNet::Response r; response(r, Fixtures::flash);
  char out[128]; auto d = diagnostics();
  CHECK(SttNet::extractGemini(r, false, out, sizeof(out), d));
  CHECK(std::string(out) == "Olá mundo" && d.model == "gemini-test-version");
  CHECK(d.status == 200 && d.error.isEmpty());
}
TEST(gemini_flash_rejects_max_tokens) {
  SttNet::Response r; response(r, Fixtures::maxTokens);
  char out[128] = "stale"; auto d = diagnostics();
  CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d));
  CHECK(out[0] == '\0' && d.error.indexOf("finishReason") >= 0);
}
TEST(gemini_flash_rejects_token_count_boundary) {
  SttNet::Response r;
  response(r, R"({"usageMetadata":{"candidatesTokenCount":10},"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"ok"}]}}]})");
  char out[32]; auto d = diagnostics();
  CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d, 10));
  CHECK(out[0] == '\0');
  CHECK(SttNet::extractGemini(r, false, out, sizeof(out), d, 11));
}
TEST(gemini_rejects_invalid_utf8) {
  for (const std::string &invalid : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                                   std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82")}) {
    SttNet::Response r;
    response(r, "{\"candidates\":[{\"finishReason\":\"STOP\",\"content\":{\"parts\":[{\"text\":\"" + invalid + "\"}]}}]}");
    char out[128] = "stale"; auto d = diagnostics();
    CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d));
    CHECK(out[0] == '\0');
  }
}
TEST(gemini_requires_complete_json_and_http_body) {
  std::vector<std::string> invalid = {std::string(Fixtures::flash) + " trailing", "{}{}",
                                     std::string(Fixtures::flash).substr(0, sizeof(Fixtures::flash) - 2)};
  for (const auto &body : invalid) {
    SttNet::Response r; response(r, body);
    char out[128] = "stale"; auto d = diagnostics();
    CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d) && out[0] == '\0');
  }
  SttNet::Response r; response(r, Fixtures::flash, false);
  char out[128]; auto d = diagnostics();
  CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d));
}
TEST(gemini_rejects_nul_and_excessive_json_depth) {
  for (const std::string &body : {std::string(R"({"text":"a\u0000b"})"),
                                std::string(33, '[') + "0" + std::string(33, ']'),
                                std::string("{\"text\":\"a\0b\"}", 14)}) {
    SttNet::Response r; response(r, body);
    SttNet::JsonDocument doc(r); CHECK(!doc.root);
  }
}
TEST(gemini_multi_part_overflow_clears_partial_output) {
  SttNet::Response r; response(r, Fixtures::flash);
  char out[10] = "stale"; auto d = diagnostics();
  CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d));
  CHECK(out[0] == '\0');
}
TEST(gemini_text_limit_rejects_without_truncating) {
  std::string body = "{\"candidates\":[{\"finishReason\":\"STOP\",\"content\":{\"parts\":[{\"text\":\"" +
                     std::string(SttNet::kMaxText, 'x') + "\"}]}}]}";
  SttNet::Response r; response(r, body);
  std::vector<char> out(SttNet::kMaxText + 100, 'Z'); auto d = diagnostics();
  CHECK(!SttNet::extractGemini(r, false, out.data(), out.size(), d));
  CHECK(out[0] == '\0' && out[1] == 'Z');
}
TEST(gemini_interactions_completed_valid_outputs) {
  SttNet::Response r; response(r, Fixtures::interaction);
  char out[128]; auto d = diagnostics();
  CHECK(SttNet::extractGemini(r, true, out, sizeof(out), d));
  CHECK(std::string(out) == "Olá mundo" && d.model == "gemini-test-version");
}
TEST(gemini_interactions_completed_empty_is_failure) {
  for (const char *body : {Fixtures::emptyInteraction,
       R"({"status":"completed","outputs":[{"type":"text","text":" \n\t"}]})",
       R"({"status":"completed","outputs":[{"type":"text","thought":true,"text":"secret"}]})",
       R"({"status":"completed","outputs":[{"type":"text"}]})"}) {
    SttNet::Response r; response(r, body);
    char out[128] = "stale"; auto d = diagnostics();
    CHECK(!SttNet::extractGemini(r, true, out, sizeof(out), d));
    CHECK(out[0] == '\0' && d.error.length());
  }
}
TEST(gemini_interactions_not_completed_is_failure) {
  SttNet::Response r; response(r, R"({"status":"in_progress","outputs":[{"type":"text","text":"partial"}]})");
  char out[128]; auto d = diagnostics();
  CHECK(!SttNet::extractGemini(r, true, out, sizeof(out), d) && out[0] == '\0');
}
TEST(gemini_model_fallback_and_validation) {
  SttNet::Response r;
  response(r, R"({"candidates":[{"finishReason":"COMPLETED","content":{"parts":[{"text":"ok"}]}}]})");
  char out[128]; auto d = diagnostics();
  CHECK(SttNet::extractGemini(r, false, out, sizeof(out), d));
  CHECK(d.model == "requested-model");
  d.reset();
  CHECK(!SttNet::extractGemini(r, false, out, sizeof(out), d) && out[0] == '\0');
}

TEST(sttnet_http_fragmented_chunked) {
  auto client = stream("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n"
                       "3;test=yes\r\nabc\r\n2\r\nde\r\n0\r\nX-Trailer: 1\r\n\r\n", 1, 1, true);
  SttNet::Response r;
  CHECK(SttNet::readResponse(client, r, 1000));
  CHECK(r.complete && r.length == 5 && std::string(r.body) == "abcde" && !r.connectionClose && !client.stopped());
}
TEST(sttnet_http_rejects_bad_framing) {
  for (const std::string wire : {
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc",
      "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naINVALID\r\n0\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n",
      "HTTP/1.1 200 OK\nContent-Length: 0\n\n"}) {
    auto client = stream(wire, 1, 1);
    SttNet::Response r;
    CHECK(!SttNet::readResponse(client, r, 1000));
    CHECK(!r.complete && r.error.length() && client.stopped());
  }
}
TEST(sttnet_http_maxbody_rejected) {
  auto client = stream("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(SttNet::kMaxBody + 1) + "\r\n\r\n", 1, 1, true);
  SttNet::Response r;
  CHECK(!SttNet::readResponse(client, r, 1000));
  CHECK(!r.complete && r.length == 0 && r.error.length());
  CHECK(!SttNet::retryable(r));
}
TEST(sttnet_204_retry_after_defers) {
  auto client = stream("HTTP/1.1 204 No Content\r\nRetry-After: 120\r\n\r\n", 1, 1, true);
  SttNet::Response r;
  CHECK(SttNet::readResponse(client, r, 1000));
  CHECK(r.status == 204 && r.complete && r.length == 0 && SttNet::retryHint(r) == 120);
  CHECK(!SttNet::retryable(r));
}
TEST(sttnet_retry_info_and_quota_zero) {
  SttNet::Response r;
  response(r, R"({"error":{"details":[{"@type":"type.googleapis.com/google.rpc.RetryInfo","retryDelay":"60.1s"}]}})", true, 429);
  CHECK(SttNet::retryHint(r) == 61 && !SttNet::retryable(r));
  r.reset();
  response(r, R"({"error":{"details":[{"violations":[{"quotaValue":"0"}]}]}})", true, 429);
  CHECK(!SttNet::retryable(r));
  r.reset(); r.status = 200; r.complete = false;
  CHECK(SttNet::retryable(r));
  r.complete = true; CHECK(!SttNet::retryable(r));
}
TEST(sttnet_base64_short_file_reads) {
  for (const auto &pair : std::vector<std::pair<std::string, std::string>>{
      {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foobar", "Zm9vYmFy"}}) {
    auto client = stream("", SIZE_MAX, 0, true);
    File file(pair.first, 1);
    CHECK(SttNet::writeFile(client, file, true) && client.sent == pair.second);
  }
}
TEST(sttnet_compatible_parsers) {
  SttNet::Response r; response(r, R"({"text":"Olá mundo","model":"whisper-test"})");
  char out[128]; auto d = diagnostics();
  CHECK(SttNet::extractCompatible(r, false, out, sizeof(out), d));
  CHECK(std::string(out) == "Olá mundo" && d.model == "whisper-test");
  r.reset(); response(r, Fixtures::compatibleSummary);
  CHECK(SttNet::extractCompatible(r, true, out, sizeof(out), d));
  CHECK(std::string(out) == "# Título\n- fato" && d.model == "gpt-test");
}
TEST(sttnet_compatible_truncation_rejected) {
  SttNet::Response r;
  response(r, R"({"choices":[{"finish_reason":"length","message":{"content":"partial"}}]})");
  char out[128] = "stale"; auto d = diagnostics();
  CHECK(!SttNet::extractCompatible(r, true, out, sizeof(out), d) && out[0] == '\0');
}
TEST(sttnet_request_allows_noncredential_query) {
  Host::enqueue(Fixtures::http("{}"));
  WiFiClientSecure client; SttNet::Response r;
  CHECK(SttNet::beginRequest(client, "example.test", 443, "GET", "/models?pageSize=100",
                            "fake-key", true, "application/json", 0, "", r));
  CHECK(Host::requests.size() == 1);
}

TEST(stt_real_client_transcribe_with_fake_fs_and_network) {
  auto cfg = compatibleSettings();
  NoteFiles::fs().put("/notes/test.wav", Fixtures::wav());
  Host::enqueue(Fixtures::http(R"({"text":"Olá mundo"})"), 1, 1);
  SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(std::string(out) == "Olá mundo" && client.lastModel() == "whisper-1");
  CHECK(Host::requests.size() == 1 && Host::responses.empty());
  CHECK(Host::requests[0].find("POST /v1/audio/transcriptions HTTP/1.1\r\n") == 0);
  CHECK(Host::requests[0].find("Authorization: Bearer fake-key\r\n") != std::string::npos);
  CHECK(Host::requests[0].find(Fixtures::wav()) != std::string::npos);
}
TEST(stt_real_client_empty_200_does_not_retry) {
  auto cfg = compatibleSettings();
  NoteFiles::fs().put("/notes/test.wav", Fixtures::wav());
  Host::enqueue(Fixtures::http(R"({"text":""})"));
  SttClient client; char out[128] = "stale";
  CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(out[0] == '\0' && Host::requests.size() == 1 && client.lastError().length());
}
TEST(stt_real_client_retries_interrupted_200) {
  auto cfg = compatibleSettings();
  NoteFiles::fs().put("/notes/test.wav", Fixtures::wav());
  Host::enqueue("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n{\"text\":\"partial");
  Host::enqueue(Fixtures::http(R"({"text":"complete"})"), 3, 1);
  SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(std::string(out) == "complete" && Host::requests.size() == 2);
}
TEST(stt_real_client_retry_after_over_60_does_not_retry) {
  auto cfg = compatibleSettings();
  Host::enqueue(Fixtures::http(R"({"error":{"message":"busy"}})", 503, "Retry-After: 120\r\n"));
  SttClient client; char out[128] = "stale";
  CHECK(!client.generateSummary(cfg, "transcrição", out, sizeof(out)));
  CHECK(out[0] == '\0' && Host::requests.size() == 1 && millis() < 1000);
}
TEST(stt_real_client_summary_complete) {
  auto cfg = compatibleSettings();
  Host::enqueue(Fixtures::http(Fixtures::compatibleSummary), 2, 1);
  SttClient client; char out[128];
  CHECK(client.generateSummary(cfg, "Uma \"nota\"\ncom ação", out, sizeof(out)));
  CHECK(std::string(out) == "# Título\n- fato" && client.lastModel() == "gpt-test");
  CHECK(Host::requests.size() == 1);
  CHECK(Host::requests[0].find("POST /v1/chat/completions HTTP/1.1\r\n") == 0);
  CHECK(Host::requests[0].find("Uma \\\"nota\\\"\\ncom ação") != std::string::npos);
}
TEST(stt_rejects_path_and_invalid_wav_without_network) {
  auto cfg = compatibleSettings(); SttClient client; char out[32];
  for (const char *path : {"/notes/../test.wav", "/other/test.wav", "/notes/missing.wav"}) {
    CHECK(!client.transcribe(cfg, path, out, sizeof(out)) && out[0] == '\0');
  }
  NoteFiles::fs().put("/notes/test.wav", std::string(60, 'x'));
  CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.empty());
}
TEST(shared_wire_mixed_2xx_errors_keepalive_one_handshake) {
  for (int status : {200, 404, 429, 503, 200}) Host::enqueue(Fixtures::http("{}", status), 1, 1, true);
  for (int status : {200, 404, 429, 503, 200}) {
    SttNet::Response r;
    CHECK(SttNet::requestJson("example.test", 443, "GET", "/models", "fake-key", true, "", r));
    CHECK(r.complete && r.status == status && !r.connectionClose);
  }
  CHECK(Host::requests.size() == 5 && Host::connectionHosts.size() == 1 && Host::responses.empty());
  for (const auto &request : Host::requests) {
    CHECK(request.find("GET /models HTTP/1.1\r\n") == 0);
    CHECK(request.find("GET /models", 1) == std::string::npos);
  }
  auto c = SyncTelemetry::counters(); CHECK(c.httpRequests == 5 && c.tlsHandshakes == 1);
}
TEST(shared_wire_early_media_4xx_closes_and_preserves_status) {
  Host::enqueue(Fixtures::http("{}"), 1, 0, true);
  Host::enqueue(Fixtures::http(R"({"error":{"status":"PERMISSION_DENIED"}})", 403), 1, 0, true);
  Host::enqueue(Fixtures::http("{}"), 1, 0, true);
  SttNet::Response first;
  CHECK(SttNet::requestJson("example.test", 443, "GET", "/models", "fake-key", true, "", first));
  auto &client = SttNet::sharedClient(); SttNet::Response error;
  CHECK(SttNet::beginRequest(client, "example.test", 443, "POST", "/audio", "fake-key", true,
                             "audio/wav", Fixtures::wav().size(), "", error));
  File audio(Fixtures::wav()); CHECK(!SttNet::writeFile(client, audio));
  CHECK(SttNet::finishRequest(client, error, false)); CHECK(error.status == 403 && error.complete && client.stopped());
  CHECK(Host::requests[1].find(Fixtures::wav()) == std::string::npos);
  SttNet::Response final;
  CHECK(SttNet::requestJson("example.test", 443, "GET", "/models", "fake-key", true, "", final));
  CHECK(Host::requests.size() == 3 && Host::connectionHosts.size() == 2);
}
TEST(shared_wire_invalid_frame_and_forced_partial_read_disconnect) {
  for (bool forced : {false, true}) {
    SttNet::endSession(); Host::resetNetwork();
    if (forced) {
      Host::enqueue(Fixtures::http("abcdef"), 2, 0, true);
      Host::responses.back().disconnectAfterRead = Fixtures::http("abcdef").size() - 3;
    } else Host::enqueue("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n", 1, 0, true);
    SttNet::Response bad;
    CHECK(!SttNet::requestJson("example.test", 443, "GET", "/bad", "fake-key", true, "", bad));
    CHECK(!bad.complete && SttNet::sharedClient().stopped());
    Host::enqueue(Fixtures::http("{}"), 1, 0, true); SttNet::Response good;
    CHECK(SttNet::requestJson("example.test", 443, "GET", "/good", "fake-key", true, "", good));
    CHECK(Host::connectionHosts.size() == 2 && Host::requests.size() == 2);
  }
}
TEST(shared_wire_destination_change_and_end_session_reconnect) {
  for (const char *host : {"a.test", "a.test", "b.test", "b.test"}) {
    Host::enqueue(Fixtures::http("{}"), 1, 0, true); SttNet::Response r;
    CHECK(SttNet::requestJson(host, 443, "GET", "/models", "fake-key", true, "", r));
  }
  CHECK(Host::connectionHosts.size() == 2 && Host::connectionHosts[0] == "a.test" && Host::connectionHosts[1] == "b.test");
  SttNet::endSession(); Host::enqueue(Fixtures::http("{}"), 1, 0, true); SttNet::Response r;
  CHECK(SttNet::requestJson("b.test", 443, "GET", "/models", "fake-key", true, "", r));
  CHECK(Host::connectionHosts.size() == 3);
}
TEST(telemetry_accrues_wait_times_wrap_and_byte_destinations) {
  Host::resetClock(UINT32_MAX - 9); SyncTelemetry::reset();
  delay(20); SyncTelemetry::phase(SyncTelemetry::Phase::GeminiUpload);
  delay(7); SyncTelemetry::wait(13, true); delay(3);
  SyncTelemetry::phase(SyncTelemetry::Phase::DriveUpload); delay(11);
  SyncTelemetry::wait(17, false); delay(5);
  SyncTelemetry::sentBytes(SyncTelemetry::Phase::GeminiUpload, 80);
  SyncTelemetry::sentBytes(SyncTelemetry::Phase::DriveUpload, 60);
  SyncTelemetry::sentBytes(SyncTelemetry::Phase::Preparing, 999);
  auto c = SyncTelemetry::counters();
  CHECK(c.preparingMs == 20 && c.geminiUploadMs == 10 && c.driveUploadMs == 16);
  CHECK(c.quotaWaitMs == 13 && c.retryWaitMs == 17 && c.geminiBytes == 80 && c.driveBytes == 60);
  CHECK(SyncTelemetry::counters().driveUploadMs == 16);
}
} // namespace

int main() {
  size_t failed = 0;
  for (const Test &test : tests()) {
    SttClient::endSession(); Host::fakeShaEnabled = false;
    Host::resetClock(); Host::resetNetwork(); Host::resetNvs(); NoteFiles::fs().clear(); SyncTelemetry::reset();
    try { test.run(); }
    catch (const std::exception &error) {
      ++failed; std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
    }
    SttClient::endSession(); SyncTelemetry::setObserver(nullptr);
  }
  std::cout << "Host tests: " << tests().size() - failed << '/' << tests().size()
            << " OK" << (failed ? " (FALHOU)" : "") << '\n';
  return failed ? 1 : 0;
}
