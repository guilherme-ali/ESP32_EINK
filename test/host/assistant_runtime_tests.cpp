#include "../../src/assistant/assistant_job.h"
#include "../../src/assistant/transport.h"
#include "../../src/assistant/live_protocol.h"
#include "../../src/audio/speech_leveler.h"
#include <functional>
#include <iostream>
using namespace AssistantHost;
using namespace Assistant;
static unsigned checks = 0, cases = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "linha " << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
static std::vector<uint8_t> processedStereoPcm(const std::vector<uint8_t> &mono);
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
  CHECK(audioStackBytes == 0 || audioStackBytes == 8192); // Required for debug AND battery.
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
  const auto expected = processedStereoPcm({0x34,0x12,0xfe,0xff,0,0x80,0xff,0x7f});
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
static std::vector<uint8_t> processedStereoPcm(const std::vector<uint8_t> &mono) {
  std::vector<uint8_t> stereo;
  SpeechLeveler p; CHECK(p.configure(24000));
  auto append = [&](int16_t value) {
    const uint8_t lo = uint8_t(value), hi = uint8_t(uint16_t(value) >> 8);
    stereo.insert(stereo.end(), {lo, hi, lo, hi});
  };
  for (size_t i = 0; i < mono.size(); i += 2) {
    const int16_t value = int16_t(uint16_t(mono[i]) | (uint16_t(mono[i + 1]) << 8));
    append(p.process(value));
  }
  int16_t value = 0;
  while (p.flush(value)) append(value);
  CHECK(p.hardGuardSamples() == 0);
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
      CHECK(playback == processedStereoPcm(head));
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
  CHECK(playback == processedStereoPcm(expected)); // Exceeds the ring: no deadlock, lost/repeated sample or wrap corruption.
  CHECK(snap.receivedAudioMs == expected.size() / 48 && snap.playedAudioMs == snap.receivedAudioMs);
  ack(job);
}
static void blePlaybackCanStartBeforeSocketTeardown() {
  ++cases; reset(); phoneRoute = true; playbackWriteLimit = 1024; closeDelayMs = 250;
  Settings s; AudioCodec codec; AssistantJob job;
  CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads > 3; })); job.release();
  CHECK(wait([] { return closingSocket.load(); }));
  CHECK(wait([] { return writeDuringClose.load(); }));
  auto snap = finish(job);
  CHECK(snap.state == AssistantJob::State::Done && snap.telemetry.phoneRoute);
  CHECK(snap.telemetry.responseReadyUs && snap.telemetry.networkDoneUs > snap.telemetry.responseReadyUs);
  CHECK(playback == processedStereoPcm(std::vector<uint8_t>{0x34,0x12,0xfe,0xff,0,0x80,0xff,0x7f}));
  ack(job);
}
static void fullBufferAblationWaitsForSocketTeardown() {
  ++cases; reset(); phoneRoute = true; earlyPlayback = false; playbackWriteLimit = 1024; closeDelayMs = 250;
  Settings s; AudioCodec codec; AssistantJob job;
  CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads > 3; })); job.release();
  CHECK(wait([] { return closingSocket.load(); }));
  sleep(80); CHECK(writes == 0);
  auto snap = finish(job);
  CHECK(snap.state == AssistantJob::State::Done && playback.size() == (4 + 120) * 4);
  CHECK(snap.telemetry.responseReadyUs && snap.telemetry.networkDoneUs > snap.telemetry.responseReadyUs);
  ack(job);
}
static std::vector<uint8_t> uploadedPcm() {
  std::vector<uint8_t> result;
  bool started = false, ended = false;
  for (const auto &message : messages()) {
    auto json = parseJson(message.data(), message.size()); CHECK(bool(json));
    const cJSON *input = field(json.get(), "realtimeInput");
    if (field(input, "activityStart")) { CHECK(!started && !ended); started = true; }
    if (const cJSON *audio = field(input, "audio")) {
      CHECK(started && !ended);
      CHECK(!strcmp(stringField(audio, "mimeType"), "audio/pcm;rate=16000"));
      const char *encoded = stringField(audio, "data"); std::vector<uint8_t> bytes;
      CHECK(unbase64(encoded, strlen(encoded), bytes, 8192));
      result.insert(result.end(), bytes.begin(), bytes.end());
    }
    if (field(input, "activityEnd")) { CHECK(started && !ended); ended = true; }
  }
  CHECK(started && ended); return result;
}
static std::vector<uint8_t> monoBytes(const std::vector<int16_t> &samples) {
  std::vector<uint8_t> bytes;
  for (int16_t sample : samples) { bytes.push_back(uint8_t(sample)); bytes.push_back(uint8_t(uint16_t(sample) >> 8)); }
  return bytes;
}
static void adoptedPrefixAndRelease() {
  ++cases;
  const uint32_t rates[] = {16000, 32000, 44100, 48000};
  for (uint32_t rate : rates) {
    reset(); Settings s; s.speakerVolumePercent = 73; AudioCodec codec; CaptureSession capture; AssistantJob job;
    // This boundary fake supplies the converter's 16-kHz output. The real
    // CaptureSession/converter suites validate resampling, not this adapter.
    for (int i = 0; i < 4097; ++i) sessionPrefix.push_back(int16_t(i * 13 - 30000));
    for (int i = 0; i < 193; ++i) sessionTail.push_back(int16_t(20000 - i * 7));
    auto expected = sessionPrefix; expected.insert(expected.end(), sessionTail.begin(), sessionTail.end());
    CHECK(capture.start(codec, rate, s.micGainDb)); CHECK(capture.inputRate() == rate);
    connectDelay = 600; fastTaskYield = true; // Avoid Windows timer granularity per block.
    CHECK(job.start(s, codec, connectCallback, &capture));
    CHECK(!job.start(s, codec, connectCallback, &capture)); CHECK(sessionEnds == 0);
    job.release(true);
    CHECK(sessionStopRequested);
    CHECK(wait([] { return sessionEnds == 1; }, 400));
    CHECK(!callbackExit && capture.finished() && capture.drained());
    CHECK(!sessionProducerAlive && captureReads == 0 && capturePreparations == 1 && sessionOutputRate == 16000);
    auto snap = finish(job);
    CHECK(snap.state == AssistantJob::State::Done && snap.released);
    CHECK(uploadedPcm() == monoBytes(expected));
    CHECK(snap.telemetry.inputBytes == expected.size() * 2 && snap.capturedMs == expected.size() / 16);
    CHECK(snap.telemetry.releaseSource == 1 && snap.telemetry.captureDoneUs < snap.telemetry.routeReadyUs);
    CHECK(playbackPreparations == 1 && playbackVolume == 73 && !playbackBeforeSessionEnd && !sessionReadAfterEnd);
    CHECK(sessionEnds == 1); ack(job); CHECK(sessionEnds == 1);
  }
}
static void adoptedStoppedAndStreamingDrain() {
  ++cases;
  for (unsigned mode = 0; mode < 2; ++mode) {
    reset(); Settings s; AudioCodec codec; CaptureSession capture; AssistantJob job;
    sessionPrefix.assign(333, -12345); sessionTail.assign(97, 23456);
    CHECK(capture.start(codec, 16000, s.micGainDb));
    if (mode == 0) { capture.requestStop(); CHECK(wait([&] { return capture.finished(); })); }
    CHECK(job.start(s, codec, connectCallback, &capture));
    if (mode == 1) { CHECK(wait([] { return sessionReads >= 4; })); CHECK(sessionEnds == 0); }
    job.release(); auto snap = finish(job);
    auto expected = sessionPrefix; expected.insert(expected.end(), sessionTail.begin(), sessionTail.end());
    CHECK(snap.state == AssistantJob::State::Done && uploadedPcm() == monoBytes(expected));
    CHECK(capture.drained() && sessionEnds == 1 && captureReads == 0 && !playbackBeforeSessionEnd && !sessionReadAfterEnd);
    ack(job);
  }
}
static void adoptedFailuresAndCancellation() {
  ++cases;
  for (unsigned mode = 0; mode < 16; ++mode) {
    reset(); Settings s; AudioCodec codec; CaptureSession capture; AssistantJob job;
    sessionPrefix.assign(2048, 777); sessionTail.assign(65, -777);
    CHECK(capture.start(codec, 16000, s.micGainDb));
    bool startOk = mode >= 10;
    if (mode == 0) s.assistantEnabled = false;
    if (mode >= 1 && mode <= 3) allocFailAt = int(mode);
    if (mode >= 4 && mode <= 6) failTaskAt = int(mode - 3);
    if (mode == 7) sessionRejectRate = true;
    if (mode == 8) sessionFail = true;
    if (mode == 9) sessionOverflow = true;
    if (mode == 10) networkUnavailable = true;
    if (mode == 11) connectDelay = 400;
    if (mode == 14) failPreparePlayback = true;
    if (mode == 15) failWrite = true;
    CHECK(job.start(s, codec, connectCallback, &capture) == startOk);
    if (mode == 11) {
      CHECK(wait([] { return callbackEntered.load(); })); job.cancel();
      CHECK(sessionStopRequested);
      CHECK(wait([] { return sessionEnds == 1; }, 150)); CHECK(!callbackExit);
    }
    if (mode == 12) sessionFail = true;
    if (mode == 13) sessionOverflow = true;
    if (mode >= 14) job.release();
    auto snap = finish(job);
    CHECK(snap.state == AssistantJob::State::Failed && strlen(snap.error));
    CHECK(sessionEnds == 1 && !sessionProducerAlive && captureReads == 0 && !playbackBeforeSessionEnd && !sessionReadAfterEnd);
    if (mode < 14) CHECK(playbackPreparations == 0);
    ack(job); CHECK(sessionEnds == 1);
  }
}
static void adoptedThirtySecondLimit() {
  ++cases; reset(); Settings s; AudioCodec codec; CaptureSession capture; AssistantJob job;
  fastTaskYield = true;
  for (int i = 0; i < 480513; ++i) sessionPrefix.push_back(int16_t(i * 17));
  CHECK(capture.start(codec, 16000, s.micGainDb));
  CHECK(job.start(s, codec, connectCallback, &capture));
  auto snap = finish(job, 15000);
  CHECK(snap.state == AssistantJob::State::Done && snap.capturedMs == 30000 && strstr(snap.detail, "30 s"));
  CHECK(snap.telemetry.releaseSource == 3 && snap.telemetry.inputBytes == 960000);
  sessionPrefix.resize(480000);
  CHECK(uploadedPcm() == monoBytes(sessionPrefix));
  CHECK(sessionEnds == 1 && sessionStopRequested && !sessionProducerAlive && captureReads == 0 && !playbackBeforeSessionEnd);
  ack(job);
}
static void adoptedResponseBackpressure() {
  ++cases;
  for (unsigned mode = 0; mode < 2; ++mode) {
    reset(); phoneRoute = true; playbackWriteLimit = 1024; fastTaskYield = true;
    Settings s; AudioCodec codec; CaptureSession capture; AssistantJob job;
    sessionPrefix.assign(1025, 1357); sessionTail.assign(33, -1357);
    std::vector<uint8_t> expected;
    if (mode == 0) { afterSetup = afterEnd; afterEnd.clear(); }
    else {
      afterEnd.clear();
      for (unsigned i = 0; i < 23; ++i) {
        std::vector<uint8_t> block(96 * 1024, uint8_t(i + 1));
        if (!afterEnd.empty()) afterEnd += '\x1e';
        afterEnd += audioMessage(block, i == 22);
        expected.insert(expected.end(), block.begin(), block.end());
      }
    }
    CHECK(capture.start(codec, 16000, s.micGainDb));
    CHECK(job.start(s, codec, connectCallback, &capture));
    if (mode == 0) {
      CHECK(wait([&] { AssistantJob::Snapshot snap; job.snapshot(snap); return snap.telemetry.firstPcmUs != 0; }));
      CHECK(writes == 0 && playbackPreparations == 0 && sessionEnds == 0);
    }
    job.release(); auto snap = finish(job, 15000);
    CHECK(snap.state == AssistantJob::State::Done && snap.bufferedAudio);
    if (mode == 1) CHECK(playback == processedStereoPcm(expected)); // Ring wrap with captureDone-dependent backpressure.
    CHECK(sessionEnds == 1 && !playbackBeforeSessionEnd && !sessionReadAfterEnd);
    auto input = sessionPrefix; input.insert(input.end(), sessionTail.begin(), sessionTail.end());
    CHECK(uploadedPcm() == monoBytes(input)); ack(job);
  }
}
static void adoptedLifetimeUntilFinished() {
  ++cases;
  for (unsigned mode = 0; mode < 3; ++mode) {
    reset(); fastTaskYield = true;
    Settings s; AudioCodec codec; AssistantJob job;
    std::unique_ptr<CaptureSession> capture(new CaptureSession);
    sessionPrefix.assign(257, 1234);
    CHECK(capture->start(codec, 16000, s.micGainDb));
    if (mode == 2) connectDelay = 500;
    CHECK(job.start(s, codec, connectCallback, capture.get())); job.release();
    if (mode == 2) {
      CHECK(wait([&] { AssistantJob::Snapshot snap; job.snapshot(snap); return snap.telemetry.captureDoneUs != 0; }));
      CHECK(!callbackExit);
      const unsigned stops = sessionStopRequests.load();
      job.release(); job.cancel(); CHECK(sessionStopRequests == stops);
      auto snap = finish(job); CHECK(snap.state == AssistantJob::State::Failed);
      CHECK(sessionStopRequests == stops); ack(job); continue;
    }
    const auto completed = finish(job); CHECK(completed.state == AssistantJob::State::Done);
    if (mode == 0) capture.reset(); // Public lifetime ends at finished, not ACK.
    else {
      sessionStopRequested = false;
      CHECK(capture->start(codec, 16000, s.micGainDb)); // Reuse before old job's ACK.
      CHECK(capture->active() && sessionProducerAlive);
    }
    const unsigned stops = sessionStopRequests.load();
    job.release(true); job.cancel(); job.release();
    CHECK(sessionStopRequests == stops);
    AssistantJob::Snapshot late; job.snapshot(late);
    CHECK(late.finished && late.state == completed.state && late.capturedMs == completed.capturedMs);
    ack(job); CHECK(sessionStopRequests == stops);
    if (mode == 1) {
      sleep(40); CHECK(capture->active() && sessionProducerAlive && !sessionStopRequested);
      capture->end(); CHECK(sessionEnds == 2);
    }
  }
}
static std::string serverWireFrame(uint8_t opcode, const std::string &payload, bool final = true) {
  CHECK(payload.size() <= 65535);
  std::string frame(1, char((final ? 0x80 : 0) | opcode));
  if (payload.size() < 126) frame += char(payload.size());
  else { frame += char(126); frame += char(payload.size() >> 8); frame += char(payload.size()); }
  return frame + payload;
}
static std::string serverClose(unsigned code, const std::string &reason = "") {
  std::string payload; payload += char(code >> 8); payload += char(code);
  return serverWireFrame(8, payload + reason);
}
static void closeAfterResponse() {
  ++cases;
  const std::string complete = "{\"serverContent\":{\"turnComplete\":true}}";
  const std::string call = "{\"toolCall\":{\"functionCalls\":[{\"id\":\"close-tool\",\"name\":\"vault_append\",\"args\":{\"path\":\"a.md\",\"text\":\"literal\"}}]}}";
  const std::vector<uint8_t> pcm = {0x34, 0x12, 0xfe, 0xff, 0, 0x80, 0xff, 0x7f};
  for (unsigned route = 0; route < 2; ++route) for (unsigned mode = 0; mode < 21; ++mode) {
    reset(); phoneRoute = route != 0; fastTaskYield = true; playbackWriteLimit = 1024;
    Settings s; AudioCodec codec; AssistantJob job;
    afterEnd = audioMessage(pcm) + '\x1e' + complete;
    wireAfterEnd = serverClose(1000); eofAfterEnd = true;
    if (mode == 1) wireAfterEnd = serverClose(1008);
    if (mode == 2) wireAfterEnd = serverClose(1011);
    if (mode == 3) wireAfterEnd = serverWireFrame(8, "");
    if (mode == 4) wireAfterEnd = serverClose(1000, std::string(1, char(0xff)));
    if (mode == 5) wireAfterEnd = serverWireFrame(8, "x");
    if (mode == 6) afterEnd = audioMessage(pcm); // No turnComplete.
    if (mode == 7) afterEnd += '\x1e' + audioMessage(pcm); // Old completion cannot cover new audio.
    if (mode == 8) afterEnd += "\x1e{\"serverContent\":{\"outputTranscription\":{\"text\":\"new segment\"}}}";
    if (mode == 9) afterEnd = complete; // No response.
    if (mode == 10) {
      afterSetup = afterEnd; wireAfterSetup = wireAfterEnd; eofAfterSetup = true;
      afterEnd.clear(); wireAfterEnd.clear(); eofAfterEnd = false; // Input not ended.
    }
    if (mode == 11) { configureGateway(s); afterEnd += '\x1e' + call; } // Tool not sent.
    if (mode == 12 || mode == 13) {
      configureGateway(s); afterEnd = call + '\x1e' + complete;
      afterToolResponse = audioMessage(pcm, mode == 13);
      wireAfterToolResponse = wireAfterEnd; eofAfterToolResponse = true;
      wireAfterEnd.clear(); eofAfterEnd = false;
    }
    if (mode == 14) wireAfterEnd.clear(); // Abrupt EOF, even after complete audio.
    if (mode == 15) wireAfterEnd = std::string("\x81\x20{", 3); // Truncated next frame.
    if (mode == 16) wireAfterEnd = serverWireFrame(1, "{", false) + serverClose(1000);
    if (mode == 17) afterEnd += "\x1e{\"error\":{\"code\":500}}";
    if (mode == 18) wireAfterEnd = serverClose(1005); // Reserved status, parser rejects it.
    if (mode == 19) {
      for (unsigned i = 0; i < 17; ++i) afterEnd += "\x1e{\"serverContent\":{\"inputTranscription\":{\"text\":\"x\"}}}";
      wireAfterEnd = serverClose(1008); // Error beyond the first 16-event slice must not be skipped.
    }
    if (mode == 20) afterEnd = "{\"serverContent\":{\"outputTranscription\":{\"text\":\"text only\"},\"turnComplete\":true}}";
    CHECK(job.start(s, codec, connectCallback));
    if (mode != 10) { CHECK(wait([] { return captureReads >= 2; })); job.release(); }
    auto snap = finish(job);
    const bool normal = mode == 0 || mode == 13 || mode == 20;
    if (normal) {
      CHECK(snap.state == AssistantJob::State::Done && snap.telemetry.responseReadyUs && wireTailRead);
      CHECK(playback == (mode == 20 ? std::vector<uint8_t>() : processedStereoPcm(pcm)));
    } else CHECK(snap.state == AssistantJob::State::Failed && strlen(snap.error));
    if (mode == 11) CHECK(hosts.size() == 1); // Close prevents starting pending effects.
    ack(job);
  }
}
static void speechPipelineAndFlushFailures() {
  ++cases;
  // Full one-second source exercises real block processing and unaligned retries.
  reset(); fastTaskYield = true; playbackWriteLimit = 127;
  Settings s; s.speakerVolumePercent = 100; AudioCodec codec; AssistantJob job;
  std::vector<int16_t> source(24000, 1000);
  source.front() = INT16_MIN; source[source.size() - 2] = INT16_MAX; source.back() = INT16_MIN;
  const auto bytes = monoBytes(source);
  afterEnd = audioMessage(bytes, true);
  CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads >= 2; })); job.release();
  auto snap = finish(job);
  CHECK(snap.state == AssistantJob::State::Done && audioStackBytes == 8192 && playbackVolume == 100);
  CHECK(playback == processedStereoPcm(bytes) && playback.size() == (source.size() + 120) * 4);
  CHECK(snap.receivedAudioMs == 1000 && snap.playedAudioMs == 1000); // Source duration, not DSP latency.
  for (size_t i = 0; i < 120 * 4; ++i) CHECK(playback[i] == 0);
  for (size_t i = 0; i < playback.size(); i += 4) {
    const int16_t value = int16_t(uint16_t(playback[i]) | (uint16_t(playback[i + 1]) << 8));
    CHECK(playback[i] == playback[i + 2] && playback[i + 1] == playback[i + 3]);
    CHECK(std::abs(int(value)) <= SpeechLeveler::ceilingPcm());
    if (i == (12000 + 120) * 4) CHECK(value > 1900 && value <= 1996);
  }
#if ARDUINO_USB_CDC_ON_BOOT
  CHECK(serialOutput.find("[PlaybackSource] kind=assistant scope=offered samples=24000 ") != std::string::npos);
  CHECK(serialOutput.find("[PlaybackProcessed] kind=assistant scope=offered samples=24120 ") != std::string::npos);
  CHECK(serialOutput.find("latency_samples=120 guards=0") != std::string::npos);
#endif
  ack(job);

  // Original 4-sample punch fixture stays RAW on transport. All failure points
  // below are AFTER its 16 output bytes, i.e. during the manual lookahead flush.
  for (unsigned mode = 0; mode < 4; ++mode) {
    reset(); fastTaskYield = true; playbackWriteLimit = 3;
    if (mode == 0) failPlaybackAfter = 16;
    else pausePlaybackAfter = 16;
    CHECK(job.start(s, codec, connectCallback)); CHECK(wait([] { return captureReads >= 2; })); job.release();
    if (mode != 0) {
      CHECK(wait([] { return playbackPaused.load(); }));
      if (mode == 1) job.cancel();
      if (mode == 2) timeOffset = 2001; // Per-write 2 s timeout.
      if (mode == 3) timeOffset = 180001; // Whole-turn deadline during flush.
    }
    snap = finish(job);
    CHECK(snap.state == AssistantJob::State::Failed && playback.size() == 16);
    CHECK(snap.receivedAudioMs == 0 && snap.playedAudioMs == 0 && playbackPreparations == 1);
    ack(job);
  }
}
int main() {
  // RFC 6455's published accept vector, using real Windows SHA-1.
  const std::string challenge = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20]; CHECK(!mbedtls_sha1_ret(reinterpret_cast<const unsigned char *>(challenge.data()), challenge.size(), digest));
  CHECK(base64(digest, 20) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  preservationAndAudioOnly(); earlyAudio(); configurationAndResources(); failuresAndTimeouts(); toolsAndCancellation(); gatewayRedirectTransport(); timeLimitsAndRedaction();
  delayedAudioPlayback(); longBufferedAudio(); blePlaybackCanStartBeforeSocketTeardown(); fullBufferAblationWaitsForSocketTeardown();
  adoptedPrefixAndRelease(); adoptedStoppedAndStreamingDrain(); adoptedFailuresAndCancellation(); adoptedThirtySecondLimit();
  adoptedResponseBackpressure();
  adoptedLifetimeUntilFinished(); closeAfterResponse();
  speechPipelineAndFlushFailures();
  std::cout << "Assistant runtime: " << cases << " grupos de cenários / " << checks << " verificações OK\n";
}
