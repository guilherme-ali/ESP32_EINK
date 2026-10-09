#include "fixtures.h"
#include "stt.h"
#include "../../src/storage/note_files.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <esp_system.h>

namespace {
struct Test { const char *name; std::function<void()> run; };
std::vector<Test> &tests() { static std::vector<Test> list; return list; }
struct Register { Register(const char *name, std::function<void()> run) { tests().push_back({name, run}); } };
#define TEST(name) void name(); Register reg_##name(#name, name); void name()
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string("linha ") + std::to_string(__LINE__) + ": " + #c); } while (false)
constexpr const char *fileName = "files/eink-1234567812345678";
constexpr const char *fileUri = "https://generativelanguage.googleapis.com/v1beta/files/eink-1234567812345678";
Settings config(bool automatic = true, const char *model = "gemini-3.5-transcribe") {
  Settings cfg; cfg.sttAutoModel = automatic; cfg.geminiFreeConfirmed = true;
  std::strcpy(cfg.sttEndpoint, "https://generativelanguage.googleapis.com/v1beta");
  std::strcpy(cfg.sttApiKey, "integration-fake-key-not-real"); std::strcpy(cfg.sttModel, model);
  Host::fakeShaEnabled = true; LittleFS.put("/notes/test.wav", Fixtures::wav()); return cfg;
}
void reply(const std::string &body, int status = 200, const std::string &headers = "") {
  Host::enqueue(Fixtures::http(body, status, headers), 7, 1, true);
  Host::responses.back().waitForBody = true; // API recusou o JSON completo
}
void discovery() {
  reply(R"({"models":[{"name":"models/gemini-3.5-transcribe"},{"name":"models/gemini-3.8-flash","outputTokenLimit":8192,"supportedGenerationMethods":["generateContent"]},{"name":"models/gemini-3.5-flash-lite","outputTokenLimit":4096,"supportedGenerationMethods":["generateContent"]}]})");
}
struct Json {
  cJSON *root;
  explicit Json(const std::string &body) : root(cJSON_ParseWithOpts(body.c_str(), nullptr, true)) { CHECK(root); }
  ~Json() { cJSON_Delete(root); }
  Json(const Json &) = delete;
  Json &operator=(const Json &) = delete;
};
std::string body(size_t index) {
  const auto &request = Host::requests.at(index); size_t end = request.find("\r\n\r\n");
  CHECK(end != std::string::npos);
  size_t length = request.find("\r\nContent-Length: "); CHECK(length < end);
  size_t expected = std::strtoull(request.c_str() + length + 18, nullptr, 10);
  CHECK(request.size() == end + 4 + expected);
  return request.substr(end + 4);
}
cJSON *field(cJSON *root, const char *name) { return cJSON_GetObjectItemCaseSensitive(root, name); }
std::string text(cJSON *root, const char *name) { auto p = field(root, name); CHECK(cJSON_IsString(p)); return p->valuestring; }
std::string decoded(const std::string &encoded) {
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string bytes; uint32_t word = 0; unsigned bits = 0;
  for (char c : encoded) {
    if (c == '=') break;
    size_t value = alphabet.find(c); CHECK(value != std::string::npos);
    word = (word << 6) | static_cast<uint32_t>(value); bits += 6;
    if (bits >= 8) { bits -= 8; bytes += static_cast<char>(word >> bits); }
  }
  return bytes;
}
void filesUpload() {
  reply("{}", 200, "X-Goog-Upload-URL: https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=fake\r\n");
  reply(std::string("{\"file\":{\"name\":\"") + fileName + "\",\"uri\":\"" + fileUri + "\",\"mimeType\":\"audio/wav\",\"state\":\"ACTIVE\"}}");
}
void filesUploadNamed(const std::string &name) {
  reply("{}", 200, "X-Goog-Upload-URL: https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=fake\r\n");
  reply("{\"file\":{\"name\":\"" + name + "\",\"uri\":\"https://generativelanguage.googleapis.com/v1beta/" + name +
        "\",\"mimeType\":\"audio/wav\",\"state\":\"ACTIVE\"}}");
}
std::string fakeFingerprint(const Settings &cfg);
void seedFullCleanup(const Settings &cfg, bool foreign = false) {
  std::string fp = foreign ? std::string(64, '0') : fakeFingerprint(cfg);
  std::string rows = "[";
  for (const char *name : {"files/old-one", "files/old-two", "files/old-three"}) {
    if (rows.size() > 1) rows += ',';
    rows += "{\"fp\":\"" + fp + "\",\"name\":\"" + name + "\",\"at\":0}";
  }
  Host::seedNvs("cleanup", rows + "]", "aiPerf");
}
void unsupportedInline() {
  reply(R"({"error":{"status":"INVALID_ARGUMENT","message":"Unsupported input.data","details":[{"fieldViolations":[{"field":"input[0].data"}]}]}})", 400);
}
void noRawKeyInNvs(const Settings &cfg) {
  CHECK(Host::nvs.count("aiPerf") && Host::nvs.at("aiPerf").count("snapshot"));
  for (const auto &ns : Host::nvs) for (const auto &entry : ns.second) {
    const auto *value = std::get_if<std::string>(&entry.second);
    if (value) CHECK(value->find(cfg.sttApiKey) == std::string::npos);
  }
  Json snapshot(std::get<std::string>(Host::nvs.at("aiPerf").at("snapshot")));
  uint8_t hash[32]; Host::fakeKeyHash(reinterpret_cast<const unsigned char *>(cfg.sttApiKey), std::strlen(cfg.sttApiKey), hash);
  std::string expected; const char hex[] = "0123456789abcdef";
  for (auto byte : hash) { expected += hex[byte >> 4]; expected += hex[byte & 15]; }
  CHECK(text(snapshot.root, "fp") == expected);
}
std::string fakeFingerprint(const Settings &cfg) {
  uint8_t digest[32]; Host::fakeKeyHash(reinterpret_cast<const unsigned char *>(cfg.sttApiKey), std::strlen(cfg.sttApiKey), digest);
  const char hex[] = "0123456789abcdef"; std::string out;
  for (auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}
void seedSnapshot(const Settings &cfg, bool correctKey, bool fresh) {
  uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  std::string rows = R"([{"model":"gemini-3.5-transcribe","seen":true,"generate":false,"limit":8192,"inlineOff":false,"inlineAt":0},{"model":"gemini-3.8-flash","seen":true,"generate":true,"limit":8192,"inlineOff":false,"inlineAt":0},{"model":"gemini-3.5-flash-lite","seen":true,"generate":true,"limit":4096,"inlineOff":false,"inlineAt":0}])";
  std::string fingerprint = correctKey ? fakeFingerprint(cfg) : std::string(64, '0');
  Host::seedNvs("snapshot", "{\"fp\":\"" + fingerprint + "\",\"cfg\":\"auto:gemini-3.5-transcribe:gemini-3.8-flash\",\"at\":" +
                 std::to_string(fresh ? now : now - 86401) + ",\"models\":" + rows + "}", "aiPerf");
}

TEST(fake_sha_defaults_to_failure_and_free_gate_precedes_network) {
  auto cfg = config(); Host::fakeShaEnabled = false; SttClient client; char out[128] = "stale";
  CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && !out[0]);
  CHECK(Host::requests.empty() && client.lastError().length());
  Host::fakeShaEnabled = true; cfg.geminiFreeConfirmed = false;
  CHECK(!client.generateSummary(cfg, "texto", out, sizeof(out)) && Host::requests.empty());
}
TEST(interactions_inline_contract_base64_output_and_snapshot) {
  auto cfg = config(); discovery(); reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(std::string(out) == "Olá mundo" && client.lastModel() == "gemini-3.5-transcribe");
  CHECK(Host::requests.size() == 2 && Host::responses.empty() && Host::connectionHosts.size() == 1);
  CHECK(Host::requests[1].find("POST /v1beta/interactions HTTP/1.1\r\n") == 0);
  CHECK(Host::requests[1].find("x-goog-api-key: " + std::string(cfg.sttApiKey)) != std::string::npos);
  Json request(body(1)); CHECK(text(request.root, "model") == "gemini-3.5-transcribe");
  auto audio = cJSON_GetArrayItem(field(request.root, "input"), 0);
  CHECK(text(audio, "type") == "audio" && text(audio, "mime_type") == "audio/wav");
  CHECK(decoded(text(audio, "data")) == Fixtures::wav() && !field(audio, "uri"));
  auto transcription = field(field(request.root, "generation_config"), "transcription_config");
  CHECK(text(field(transcription, "mode"), "type") == "verbatim");
  auto language = cJSON_GetArrayItem(field(transcription, "language_codes"), 0);
  CHECK(cJSON_IsString(language) && std::string(language->valuestring) == "pt-BR");
  noRawKeyInNvs(cfg);
}
TEST(flash_inline_and_markdown_reuse_discovery_and_tls_low_config) {
  auto cfg = config(false, "gemini-3.8-flash"); discovery(); reply(Fixtures::flash); reply(Fixtures::flash);
  std::vector<uint32_t> calls;
  Host::onConnect = [&calls](size_t index) { if (index) calls.push_back(millis()); };
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(client.generateSummary(cfg, "Uma \"nota\"\ncom ação", out, sizeof(out)));
  CHECK(std::string(out) == "Olá mundo" && client.lastModel() == "gemini-test-version");
  CHECK(Host::requests.size() == 3 && Host::connectionHosts.size() == 1);
  Json audio(body(1)); auto contents = cJSON_GetArrayItem(field(audio.root, "contents"), 0);
  auto part = cJSON_GetArrayItem(field(contents, "parts"), 1);
  CHECK(decoded(text(field(part, "inlineData"), "data")) == Fixtures::wav());
  auto generation = field(audio.root, "generationConfig");
  CHECK(text(field(generation, "thinkingConfig"), "thinkingLevel") == "LOW");
  Json markdown(body(2)); contents = cJSON_GetArrayItem(field(markdown.root, "contents"), 0);
  CHECK(text(cJSON_GetArrayItem(field(contents, "parts"), 1), "text") == "Uma \"nota\"\ncom ação");
  CHECK(calls.size() == 2 && calls[1] - calls[0] >= 13000);
  CHECK(SyncTelemetry::counters().quotaWaitMs > 0);
}
TEST(lite_never_gets_flash_thinking_low) {
  auto cfg = config(false, "gemini-3.5-flash-lite"); std::strcpy(cfg.summaryModel, "gemini-3.5-flash-lite");
  discovery(); reply(Fixtures::flash); reply(Fixtures::flash); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(client.generateSummary(cfg, "texto", out, sizeof(out)));
  for (size_t index : {size_t(1), size_t(2)}) {
    Json request(body(index)); CHECK(!field(field(request.root, "generationConfig"), "thinkingConfig"));
  }
  CHECK(Host::connectionHosts.size() == 1);
}
TEST(unsupported_inline_400_files_uri_same_preferred_and_deferred_cleanup) {
  auto cfg = config(); discovery(); unsupportedInline(); filesUpload(); reply(Fixtures::interactionSteps);
  bool reservedBeforeFiles = false;
  Host::onConnect = [&reservedBeforeFiles](size_t index) {
    if (index == 2) {
      CHECK(Host::nvs.at("aiPerf").count("cleanup"));
      CHECK(std::get<std::string>(Host::nvs.at("aiPerf").at("cleanup")).find(fileName) != std::string::npos);
      reservedBeforeFiles = true;
    }
  };
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(reservedBeforeFiles && std::string(out) == "Olá mundo");
  CHECK(Host::requests.size() == 5 && Host::connectionHosts.size() == 1);
  CHECK(Host::requests[2].find("POST /upload/v1beta/files ") == 0);
  CHECK(body(3) == Fixtures::wav()); Json uri(body(4));
  CHECK(text(uri.root, "model") == "gemini-3.5-transcribe");
  CHECK(text(cJSON_GetArrayItem(field(uri.root, "input"), 0), "uri") == fileUri);
  for (const auto &request : Host::requests) CHECK(request.find("DELETE ") != 0);
  noRawKeyInNvs(cfg);
  reply(R"({"error":{"status":"RESOURCE_EXHAUSTED"}})", 429, "Retry-After: 120\r\n");
  auto deliveredModel = client.lastModel(); SttClient::cleanupPending(cfg);
  CHECK(client.lastModel() == deliveredModel && client.lastStatusCode() == 200 && client.lastError().isEmpty());
  CHECK(Host::requests.size() == 6 && Host::requests[5].find("DELETE /v1beta/files/") == 0);
  CHECK(Host::nvs.at("aiPerf").count("cleanup") && std::get<std::string>(Host::nvs.at("aiPerf").at("cleanup")).find(fileName) != std::string::npos);
  SttClient::endSession(); SttClient::cleanupPending(cfg); CHECK(Host::requests.size() == 6);
}
TEST(inline_200_empty_falls_back_to_uri_without_false_completion) {
  auto cfg = config(); discovery(); reply(Fixtures::emptyInteraction); filesUpload(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 5 && client.lastModel() == "gemini-3.5-transcribe");
  Json uri(body(4)); CHECK(text(cJSON_GetArrayItem(field(uri.root, "input"), 0), "uri") == fileUri);
  reply("", 204); SttClient::cleanupPending(cfg);
  CHECK(Host::requests.size() == 6 && std::get<std::string>(Host::nvs.at("aiPerf").at("cleanup")) == "[]");
}
TEST(inline_400_unrelated_error_does_not_upload_files) {
  auto cfg = config(false); discovery(); reply(R"({"error":{"status":"INVALID_ARGUMENT","message":"bad language_codes"}})", 400);
  SttClient client; char out[128] = "stale";
  CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && !out[0]);
  CHECK(Host::requests.size() == 2 && Host::responses.empty());
  for (const auto &request : Host::requests) CHECK(request.find("/upload/") == std::string::npos);
}
TEST(inline_invalid_json_is_not_transport_compatibility_probe) {
  auto cfg = config(false); discovery(); reply("{malformed"); SttClient client; char out[128] = "stale";
  CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && !out[0]);
  CHECK(Host::requests.size() == 2);
}
TEST(account_403_blocks_enumeration_and_credential_rotation_recovers) {
  auto cfg = config(); reply(R"({"error":{"status":"PERMISSION_DENIED","message":"do not leak integration-fake-key-not-real"}})", 403);
  SttClient client; char out[128]; CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 1 && client.lastStatusCode() == 403);
  CHECK(client.lastError().indexOf(cfg.sttApiKey) < 0);
  CHECK(!client.generateSummary(cfg, "texto", out, sizeof(out)) && Host::requests.size() == 1);
  std::strcpy(cfg.sttApiKey, "rotated-integration-fake"); discovery(); reply(Fixtures::interactionSteps);
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && Host::requests.size() == 3);
}
TEST(discovery_pagination_encodes_token_and_does_not_expand_allowlist) {
  auto cfg = config();
  reply(R"({"models":[{"name":"models/expensive-not-allowed","supportedGenerationMethods":["generateContent"]}],"nextPageToken":"a+/="})");
  discovery(); reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 3 && Host::requests[1].find("pageToken=a%2B%2F%3D") != std::string::npos);
  CHECK(Host::requests[2].find("POST /v1beta/interactions") == 0 && Host::connectionHosts.size() == 1);
  noRawKeyInNvs(cfg);
}
TEST(discovery_cache_survives_end_session_and_expires_after_day) {
  auto cfg = config(); discovery(); reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  SttClient::endSession(); reply(Fixtures::interactionSteps);
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 3 && Host::connectionHosts.size() == 2);
  delay(24U * 60 * 60 * 1000 + 1); discovery(); reply(Fixtures::interactionSteps);
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 5 && Host::requests[3].find("GET /v1beta/models?") == 0);
}
TEST(nvs_snapshot_same_fingerprint_restores_discovery_without_http) {
  auto cfg = config(); seedSnapshot(cfg, true, true); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 1 && Host::requests[0].find("POST /v1beta/interactions") == 0);
  noRawKeyInNvs(cfg);
}
TEST(nvs_snapshot_wrong_fingerprint_requires_new_discovery) {
  auto cfg = config(); seedSnapshot(cfg, false, true); discovery(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 2 && Host::requests[0].find("GET /v1beta/models?") == 0);
  noRawKeyInNvs(cfg);
}
TEST(nvs_snapshot_expired_requires_new_discovery) {
  auto cfg = config(); seedSnapshot(cfg, true, false); discovery(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 2 && Host::requests[0].find("GET /v1beta/models?") == 0);
}
TEST(nvs_snapshot_write_failure_does_not_prevent_valid_delivery) {
  auto cfg = config(); Host::failPuts.insert("aiPerf/snapshot"); discovery(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && std::string(out) == "Olá mundo");
  CHECK((!Host::nvs.count("aiPerf") || !Host::nvs.at("aiPerf").count("snapshot")) && Host::requests.size() == 2);
}
TEST(files_cleanup_timeout_bounded_after_delivery_and_diagnostic_preserved) {
  auto cfg = config(); discovery(); unsupportedInline(); filesUpload(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  uint32_t deliveredAt = millis(); Host::enqueue("", 1, 0, true);
  SttClient::cleanupPending(cfg); CHECK(millis() - deliveredAt <= 15000);
  CHECK(std::string(out) == "Olá mundo" && client.lastStatusCode() == 200 && client.lastError().isEmpty());
  CHECK(Host::requests.size() == 6 && Host::connectionHosts.size() == 1);
  CHECK(std::get<std::string>(Host::nvs.at("aiPerf").at("cleanup")).find(fileName) != std::string::npos);
}
TEST(partial_inference_response_retries_on_new_connection) {
  auto cfg = config(false); discovery();
  Host::enqueue(Fixtures::http(Fixtures::interactionSteps), 7, 1, true);
  Host::responses.back().disconnectAfterRead = Fixtures::http(Fixtures::interactionSteps).size() - 12;
  reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && std::string(out) == "Olá mundo");
  CHECK(Host::requests.size() == 3 && Host::connectionHosts.size() == 2);
}
TEST(flash_max_tokens_is_failure_not_complete_text) {
  auto cfg = config(false, "gemini-3.8-flash"); discovery(); reply(Fixtures::maxTokens);
  SttClient client; char out[128] = "stale";
  CHECK(!client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && !out[0]);
  CHECK(Host::requests.size() == 2 && client.lastError().length());
}
TEST(markdown_and_discovery_bytes_do_not_train_audio_upload_rate) {
  auto cfg = config(); discovery(); reply(Fixtures::interactionSteps); reply(Fixtures::flash);
  SttClient client; char out[128]; SttClient::beginBatch();
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  uint64_t audioBytes = SyncTelemetry::counters().geminiBytes;
  CHECK(audioBytes == body(1).size());
  CHECK(client.generateSummary(cfg, "Uma nota sobre uma tarefa", out, sizeof(out)));
  CHECK(SyncTelemetry::counters().geminiBytes == audioBytes);
}
TEST(four_files_uploads_in_one_batch_recover_capacity_without_losing_delivery) {
  auto cfg = config(); LittleFS.put("/notes/test.wav", Fixtures::sizedWav(2 * 1024 * 1024 + 44));
  discovery(); SttClient client; SttClient::beginBatch(); char out[128];
  for (unsigned i = 0; i < 4; ++i) {
    uint32_t random = 0x12345670U + i;
    Host::randomValues.push_back(random); Host::randomValues.push_back(random);
    char name[64]; std::snprintf(name, sizeof(name), "files/eink-%08x%08x", random, random);
    if (i == 3) reply("", 204);
    filesUploadNamed(name); reply(Fixtures::interactionSteps);
    CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)) && std::string(out) == "Olá mundo");
  }
  CHECK(Host::requests.size() == 14);
  CHECK(Host::requests[10].find("DELETE /v1beta/files/eink-1234567012345670") == 0);
  Json pending(std::get<std::string>(Host::nvs.at("aiPerf").at("cleanup")));
  CHECK(cJSON_GetArraySize(pending.root) == 3 && Host::responses.empty());
}
TEST(full_restored_files_queue_with_zero_epoch_can_reconnect_and_recover) {
  auto cfg = config(); seedSnapshot(cfg, true, true); seedFullCleanup(cfg);
  LittleFS.put("/notes/test.wav", Fixtures::sizedWav(2 * 1024 * 1024 + 44));
  reply("", 404); filesUpload(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; SttClient::beginBatch();
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 4 && Host::requests[0].find("DELETE /v1beta/files/old-one") == 0);
  CHECK(Host::connectionHosts.size() == 1 && Host::responses.empty());
}
TEST(cleanup_budget_is_shared_across_calls_and_not_renewed_per_note) {
  auto cfg = config(); discovery(); unsupportedInline(); filesUpload(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; SttClient::beginBatch();
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  Host::enqueue("", 1, 0, true); uint32_t start = millis();
  SttClient::cleanupPending(cfg); size_t requests = Host::requests.size();
  SttClient::cleanupPending(cfg);
  CHECK(millis() - start <= 15000 && Host::requests.size() == requests);
  CHECK(client.lastStatusCode() == 200 && std::string(out) == "Olá mundo");
}
TEST(foreign_fingerprint_cleanup_does_not_block_or_delete_with_new_key) {
  auto cfg = config(); seedSnapshot(cfg, true, true); seedFullCleanup(cfg, true);
  LittleFS.put("/notes/test.wav", Fixtures::sizedWav(2 * 1024 * 1024 + 44));
  filesUpload(); reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 3);
  for (const auto &request : Host::requests) CHECK(request.find("DELETE ") != 0);
  CHECK(Host::nvs.at("aiPerf").count("retired"));
  CHECK(std::get<std::string>(Host::nvs.at("aiPerf").at("retired")).find("api_expiry_key_changed") != std::string::npos);
  noRawKeyInNvs(cfg);
}
TEST(future_inline_rejection_timestamp_remains_blocked_after_clock_regression) {
  auto cfg = config(); seedSnapshot(cfg, true, true);
  auto &saved = std::get<std::string>(Host::nvs.at("aiPerf").at("snapshot"));
  size_t flag = saved.find("\"inlineOff\":false"); CHECK(flag != std::string::npos);
  saved.replace(flag, std::strlen("\"inlineOff\":false"), "\"inlineOff\":true");
  size_t at = saved.find("\"inlineAt\":0"); CHECK(at != std::string::npos);
  saved.replace(at, std::strlen("\"inlineAt\":0"), "\"inlineAt\":" + std::to_string(std::time(nullptr) + 600));
  filesUpload(); reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 3 && Host::requests[0].find("POST /upload/v1beta/files") == 0);
  Json request(body(2)); auto audio = cJSON_GetArrayItem(field(request.root, "input"), 0);
  CHECK(field(audio, "uri") && !field(audio, "data"));
}
TEST(inline_transport_failure_uses_files_uri_in_same_model_without_hammering) {
  auto cfg = config(); discovery(); Host::enqueue("", 1, 0, false);
  filesUpload(); reply(Fixtures::interactionSteps); SttClient client; char out[128];
  CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  CHECK(Host::requests.size() == 5 && client.lastModel() == "gemini-3.5-transcribe");
  Json request(body(4)); CHECK(text(request.root, "model") == "gemini-3.5-transcribe");
  CHECK(field(cJSON_GetArrayItem(field(request.root, "input"), 0), "uri"));
  Json cached(std::get<std::string>(Host::nvs.at("aiPerf").at("snapshot")));
  auto model = cJSON_GetArrayItem(field(cached.root, "models"), 0);
  CHECK(!cJSON_IsTrue(field(model, "inlineOff")));
}
TEST(inline_multiblock_wave_has_correct_length_and_continuous_base64) {
  auto cfg = config(); auto audio = Fixtures::sizedWav(141356);
  LittleFS.put("/notes/test.wav", audio); discovery(); reply(Fixtures::interactionSteps);
  SttClient client; char out[128]; CHECK(client.transcribe(cfg, "/notes/test.wav", out, sizeof(out)));
  Json request(body(1)); auto part = cJSON_GetArrayItem(field(request.root, "input"), 0);
  auto encoded = text(part, "data");
  CHECK(encoded.size() == 4 * ((audio.size() + 2) / 3));
  CHECK(decoded(encoded) == audio && encoded.find('=') >= encoded.size() - 2);
}
}

// Cada caso roda em processo novo: endSession fecha TLS, mas deliberadamente
// conserva cache e fila Files de producao. Nao inventamos reset SDK no fake.
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--list") {
    for (const auto &test : tests()) std::cout << test.name << '\n';
    return 0;
  }
  if (argc != 2) return 2;
  size_t index = std::strtoull(argv[1], nullptr, 10); if (index >= tests().size()) return 2;
  Host::resetClock(); Host::resetNetwork(); Host::resetNvs(); LittleFS.reset(); SyncTelemetry::reset();
  try { tests()[index].run(); SttClient::endSession(); return 0; }
  catch (const std::exception &e) {
    SttClient::endSession(); std::cout << "FAIL " << tests()[index].name << ": " << e.what() << '\n'; return 1;
  }
}
