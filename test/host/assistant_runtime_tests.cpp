#include "../../src/assistant/assistant_job.h"
#include "../../src/assistant/transport.h"
#include "../../src/assistant/live_protocol.h"
#include <functional>
#include <iostream>
using namespace AssistantHost;
using namespace Assistant;
static unsigned checks = 0, cases = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "linha " << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
static bool wait(const std::function<bool()> &test, unsigned ms = 7000) {
  auto begin = std::chrono::steady_clock::now();
  do {
    if (test()) return true;
    sleep(2);
  } while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(ms));
  return false;
}
static AssistantJob::Snapshot finish(AssistantJob &job, unsigned timeoutMs = 7000) {
  AssistantJob::Snapshot s;
  const bool done = wait([&] { job.snapshot(s); return s.finished; }, timeoutMs);
  if (!done) std::cerr << "grupo=" << cases << " estado=" << static_cast<int>(s.state)
    << " detalhe=" << s.detail << " erro=" << s.error << " rx=" << s.receivedAudioMs << " tx=" << s.playedAudioMs << "\n";
  CHECK(done);
  CHECK(!job.active()); CHECK(tasksAlive == 0); CHECK(socketsAlive == 0); CHECK(!codecOn); CHECK(!clockRace);
  return s;
}
static void ack(AssistantJob &job) {
  CHECK(job.acknowledgeFinished()); AssistantJob::Snapshot s; job.snapshot(s);
  CHECK(s.state == AssistantJob::State::Idle && !s.finished); CHECK(!job.acknowledgeFinished());
}
static void configureGateway(Settings &s) {
  strcpy(s.assistantGatewayUrl, "https://script.google.com/macros/s/test_deployment/exec");
  strcpy(s.assistantGatewayToken, "secret_gateway_token_0123456789abcdef");
}
static void preservationAndAudioOnly() {
  reset(); ++cases; Settings s; AudioCodec codec; AssistantJob job;
  connectDelay = 1500;
  auto begin = std::chrono::steady_clock::now(); CHECK(job.start(s, codec, connectCallback));
  CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));
  CHECK(wait([] { return callbackEntered && captureReads >= 5; }));
  CHECK(!callbackExit); CHECK(!job.acknowledgeFinished());
  AssistantJob::Snapshot snap; job.snapshot(snap); CHECK(snap.capturedMs > 0 && !snap.finished);
  job.release(); snap = finish(job); CHECK(snap.state == AssistantJob::State::Done && snap.released);
  auto sent = messages(); size_t audioBytes = 0; bool start = false, end = false;
  for (const auto &message : sent) {
    auto json = parseJson(message.data(), message.size()); CHECK(bool(json));
    const cJSON *realtime = field(json.get(), "realtimeInput");
    if (field(realtime, "activityStart")) { CHECK(!start && !audioBytes && !end); start = true; }
    if (const cJSON *audio = field(realtime, "audio")) {
      CHECK(start && !end);
      std::vector<uint8_t> pcm; const char *b64 = stringField(audio, "data");
      CHECK(unbase64(b64, strlen(b64), pcm, 2048));
      for (size_t i = 0; i < pcm.size(); i += 2) CHECK(pcm[i] == 0x34 && pcm[i + 1] == 0x12);
      audioBytes += pcm.size();
    }
    if (field(realtime, "activityEnd")) { CHECK(start && audioBytes && !end); end = true; }
  }
  CHECK(start && end); CHECK(audioBytes / 32 == snap.capturedMs);
  const std::vector<uint8_t> expected = {0x34,0x12,0x34,0x12,0xfe,0xff,0xfe,0xff,0,0x80,0,0x80,0xff,0x7f,0xff,0x7f};
  CHECK(playback == expected); CHECK(rateChanges == 1 && writes > 1); CHECK(!strlen(snap.text));
  ack(job);
  // Same object can start again only after acknowledgement.
  reset(); CHECK(job.start(s, codec, connectCallback)); CHECK(!job.start(s, codec, connectCallback));
  CHECK(wait([] { return captureReads > 2; })); job.cancel(); snap = finish(job);
  CHECK(snap.state == AssistantJob::State::Failed); ack(job);
}
static void earlyAudio() {
  reset(); ++cases; Settings s; AudioCodec codec; AssistantJob job;
  afterSetup = afterEnd; afterEnd.clear();
  CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return messages().size() >= 2; }));
  sleep(30); CHECK(writes == 0 && rateChanges == 0 && codecOn);
  job.release(); auto snap = finish(job); CHECK(snap.state == AssistantJob::State::Done); CHECK(!playback.empty()); ack(job);
}
static void configurationAndResources() {
  ++cases;
  for (unsigned mode = 0; mode < 5; ++mode) {
    reset(); Settings s; AudioCodec codec; AssistantJob job;
    if (mode == 0) s.geminiFreeConfirmed = false;
    if (mode == 1) strcpy(s.assistantModel, "gemini-3.8-pro");
    if (mode == 2) memset(s.assistantModel, 'x', sizeof(s.assistantModel));
    if (mode == 3) s.assistantEnabled = false;
    if (mode == 4) *s.sttApiKey = 0;
    CHECK(!job.start(s, codec, connectCallback)); auto snap = finish(job);
    CHECK(snap.state == AssistantJob::State::Failed); CHECK(hosts.empty() && taskCount == 0); ack(job);
  }
  for (int index = 1; index <= 3; ++index) {
    reset(); Settings s; AudioCodec codec; AssistantJob job; failTaskAt = index;
    CHECK(!job.start(s, codec, connectCallback)); auto snap = finish(job);
    CHECK(snap.state == AssistantJob::State::Failed); ack(job);
    reset(); allocFailAt = index;
    CHECK(!job.start(s, codec, connectCallback)); snap = finish(job);
    CHECK(snap.state == AssistantJob::State::Failed); CHECK(taskCount == 0); ack(job);
  }
}
static void failuresAndTimeouts() {
  ++cases;
  for (unsigned mode = 0; mode < 5; ++mode) {
    reset(); Settings s; AudioCodec codec; AssistantJob job;
    if (mode == 0) networkUnavailable = true;
    if (mode == 1) wrongAccept = true;
    if (mode == 2) failRead = true;
    if (mode == 3) failWrite = true;
    CHECK(job.start(s, codec, connectCallback));
    if (mode == 3) { CHECK(wait([] { return captureReads >= 2; })); job.release(); }
    if (mode == 4) { CHECK(wait([] { return callbackEntered.load(); })); timeOffset = 180001; }
    auto snap = finish(job); CHECK(snap.state == AssistantJob::State::Failed && strlen(snap.error));
    CHECK(!strstr(snap.error, s.sttApiKey) && !strstr(snap.error, "wss://")); ack(job);
  }
}
static void toolsAndCancellation() {
  ++cases;
  const std::string call = "{\"toolCall\":{\"functionCalls\":[{\"id\":\"original:id/42\",\"name\":\"vault_append\",\"args\":{\"path\":\"a.md\",\"text\":\"literal\"}}]}}";
  for (unsigned mode = 0; mode < 7; ++mode) {
    reset(); Settings s; configureGateway(s); AudioCodec codec; AssistantJob job;
    afterToolResponse = afterEnd; afterEnd = call;
    if (mode == 1) afterEnd += std::string(1, '\x1e') + "{\"toolCallCancellation\":{\"ids\":[\"original:id/42\"]}}";
    if (mode == 2) { afterEnd = "{\"toolCallCancellation\":{\"ids\":[\"original:id/42\"]}}" + std::string(1, '\x1e') + call; }
    if (mode == 3) gatewayDelay = 300;
    if (mode == 4) afterEnd += std::string(1, '\x1e') + call;
    if (mode == 5) afterEnd = "{\"toolCall\":{\"functionCalls\":[{\"id\":\"original:id/42\",\"name\":\"unavailable_tool\",\"args\":{\"query\":\"hoje\"}}]}}";
    if (mode == 6) gatewayLoseFirst = true;
    CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads >= 4; })); job.release();
    if (mode == 3) {
      CHECK(wait([] { std::lock_guard<std::mutex> lock(mutex); return hosts.size() >= 2; }));
      job.cancel(); AssistantJob::Snapshot pending; job.snapshot(pending); CHECK(!pending.finished);
    }
    auto snap = finish(job);
    if (mode == 3) { CHECK(snap.state == AssistantJob::State::Failed); ack(job); continue; }
    CHECK(snap.state == AssistantJob::State::Done);
    unsigned posts = 0, responses = 0;
    for (const auto &request : requests) if (request.compare(0, 5, "POST ") == 0) ++posts;
    CHECK(posts == (mode == 6 ? 2U : mode == 0 || mode == 4 ? 1U : 0U));
    if (mode == 6) {
      std::vector<std::string> mutations;
      for (const auto &request : requests) if (request.compare(0, 5, "POST ") == 0) mutations.push_back(request);
      CHECK(mutations.size() == 2 && mutations[0] == mutations[1]);
    }
    for (const auto &message : messages()) {
      auto json = parseJson(message.data(), message.size());
      const cJSON *response = field(json.get(), "toolResponse"); if (!response) continue;
      ++responses;
      const cJSON *entry = cJSON_GetArrayItem(field(response, "functionResponses"), 0);
      CHECK(!strcmp(stringField(entry, "id"), "original:id/42"));
      CHECK(trueField(field(entry, "response"), "ok") == (mode == 0 || mode == 4 || mode == 6));
      if (mode == 5) CHECK(!strcmp(stringField(field(field(entry, "response"), "error"), "code"), "UNKNOWN_TOOL"));
    }
    CHECK(responses == 1); ack(job);
  }
}
static bool neverStop(void *) { return false; }
static void gatewayRedirectTransport() {
  ++cases;
  const std::string id = gatewayId(1, 0), token(40, 's');
  const std::string payload = gatewayRequest(token, id, "vault_append", "{\"path\":\"a.md\",\"text\":\"literal\"}");
  reset();
  std::string exhausted = ToolGateway::execute("https://script.google.com/macros/s/test/exec", payload, id, neverStop, nullptr, 1000);
  CHECK(exhausted.find("CANCELLED") != std::string::npos && hosts.empty() && requests.empty());
  for (unsigned mode = 0; mode < 6; ++mode) {
    reset();
    redirectUrl = mode == 0 ? "https://script.googleusercontent.com/macros/echo?nonce=abc" : "https://evil.example/steal";
    if (mode == 2) { redirectUrl.clear(); gatewayBadJson = true; }
    if (mode == 3) redirectUrl = "https://script.googleusercontent.com/echo?token=" + token;
    if (mode == 4) { redirectForever = true; redirectUrl = "https://script.googleusercontent.com/echo?nonce=abc"; }
    if (mode == 5) {
      redirectUrl.clear();
      gatewayBody = "{\"version\":1,\"id\":null,\"ok\":false,\"error\":{\"code\":\"UNAUTHORIZED\",\"message\":\"Autenticação inválida.\"}}";
    }
    auto result = ToolGateway::execute("https://script.google.com/macros/s/test/exec", payload, id, neverStop, nullptr);
    auto json = parseJson(result.data(), result.size()); CHECK(bool(json));
    CHECK(trueField(json.get(), "ok") == (mode == 0));
    CHECK(socketsAlive == 0);
    unsigned posts = 0, gets = 0;
    for (const auto &request : requests) {
      if (request.compare(0, 5, "POST ") == 0) { ++posts; CHECK(request.find(token) != std::string::npos); }
      if (request.compare(0, 4, "GET ") == 0) { ++gets; CHECK(request.find(token) == std::string::npos); CHECK(request.find(payload) == std::string::npos); }
    }
    CHECK(posts == 1); CHECK(gets == (mode == 0 ? 1U : mode == 4 ? 3U : 0U));
    if (mode == 1) CHECK(hosts.size() == 1 && result.find("UNSAFE_REDIRECT") != std::string::npos);
    if (mode == 5) CHECK(result.find("UNAUTHORIZED") != std::string::npos);
  }
}
static bool hasMessage(const char *needle) {
  for (const auto &message : messages()) if (message.find(needle) != std::string::npos) return true;
  return false;
}
static void timeLimitsAndRedaction() {
  ++cases;
  for (unsigned mode = 0; mode < 3; ++mode) {
    reset(); Settings s; configureGateway(s); AudioCodec codec; AssistantJob job;
    if (mode == 1) afterEnd.clear();
    if (mode == 2) afterEnd = "{\"serverContent\":{\"outputTranscription\":{\"text\":\"test_API_key secret_gateway_token_0123456789abcdef wss://host/?key=secret\"},\"turnComplete\":true}}";
    CHECK(job.start(s, codec, connectCallback));
    CHECK(wait([] { return captureReads > 3 && hasMessage("activityStart") && hasMessage("\"audio\""); }));
    if (mode == 0) timeOffset = 30001;
    else job.release();
    if (mode == 1) { CHECK(wait([] { return hasMessage("activityEnd"); })); sleep(30); timeOffset = 45001; }
    auto snap = finish(job);
    if (mode == 0) CHECK(snap.state == AssistantJob::State::Done && strstr(snap.detail, "30 s"));
    if (mode == 1) CHECK(snap.state == AssistantJob::State::Failed && strstr(snap.error, "45 s"));
    if (mode == 2) {
      CHECK(snap.state == AssistantJob::State::Done && strlen(snap.text));
      CHECK(!strstr(snap.text, s.sttApiKey) && !strstr(snap.text, s.assistantGatewayToken) && !strstr(snap.text, "wss://"));
    }
    ack(job);
  }
}
static std::string audioMessage(const std::vector<uint8_t> &pcm, bool complete = false) {
  return "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"" +
    base64(pcm.data(), pcm.size()) + "\"}}]}" + (complete ? ",\"turnComplete\":true}}" : "}}");
}
static std::vector<uint8_t> stereoPcm(const std::vector<uint8_t> &mono) {
  std::vector<uint8_t> stereo;
  for (size_t i = 0; i < mono.size(); i += 2)
    stereo.insert(stereo.end(), {mono[i], mono[i + 1], mono[i], mono[i + 1]});
  return stereo;
}
static void delayedAudioPlayback() {
  ++cases;
  for (unsigned mode = 0; mode < 3; ++mode) {
    reset(); phoneRoute = mode != 1; playbackWriteLimit = 1024;
    Settings s; AudioCodec codec; AssistantJob job;
    std::vector<uint8_t> head(mode == 1 ? 30000 : 2048, 0x21), tail(3072, 0x54);
    afterEnd = audioMessage(head); delayedAfterEnd = audioMessage(tail, true);
    CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads > 3; })); job.release();
    CHECK(wait([] { return responseHeadRead.load(); }));
    if (mode == 1) CHECK(wait([] { return writes > 0; })); // Wi-Fi remains progressive.
    else { sleep(250); CHECK(writes == 0 && rateChanges == 0); }
    if (mode == 2) job.cancel();
    else releaseResponseTail = true;
    auto snap = finish(job);
    CHECK(snap.bufferedAudio == (mode != 1));
    if (mode == 2) CHECK(snap.state == AssistantJob::State::Failed && playback.empty());
    else {
      CHECK(snap.state == AssistantJob::State::Done);
      head.insert(head.end(), tail.begin(), tail.end());
      CHECK(playback == stereoPcm(head));
      CHECK(snap.receivedAudioMs == head.size() / 48 && snap.playedAudioMs == snap.receivedAudioMs);
      if (mode == 0) CHECK(snap.rebufferCount == 0);
    }
    ack(job);
  }
}
static void longBufferedAudio() {
  ++cases; reset(); phoneRoute = true; playbackWriteLimit = 1024; fastTaskYield = true;
  Settings s; AudioCodec codec; AssistantJob job;
  std::vector<uint8_t> expected;
  afterEnd.clear();
  for (unsigned block = 0; block < 24; ++block) {
    std::vector<uint8_t> pcm(96 * 1024, static_cast<uint8_t>(block + 1));
    if (!afterEnd.empty()) afterEnd += '\x1e';
    afterEnd += audioMessage(pcm, block == 23);
    expected.insert(expected.end(), pcm.begin(), pcm.end());
  }
  CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads > 3; })); job.release();
  auto snap = finish(job, 15000);
  CHECK(snap.state == AssistantJob::State::Done && snap.bufferedAudio);
  CHECK(playback == stereoPcm(expected)); // Exceeds the ring: no deadlock, lost/repeated sample or wrap corruption.
  CHECK(snap.receivedAudioMs == expected.size() / 48 && snap.playedAudioMs == snap.receivedAudioMs);
  ack(job);
}
int main() {
  // RFC 6455's published accept vector, using real Windows SHA-1.
  const std::string challenge = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20]; CHECK(!mbedtls_sha1_ret(reinterpret_cast<const unsigned char *>(challenge.data()), challenge.size(), digest));
  CHECK(base64(digest, 20) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  preservationAndAudioOnly(); earlyAudio(); configurationAndResources(); failuresAndTimeouts(); toolsAndCancellation(); gatewayRedirectTransport(); timeLimitsAndRedaction();
  delayedAudioPlayback(); longBufferedAudio();
  std::cout << "Assistant runtime: " << cases << " grupos de cenários / " << checks << " verificações OK\n";
}
