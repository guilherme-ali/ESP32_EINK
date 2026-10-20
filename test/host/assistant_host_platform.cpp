#include "assistant_host_platform.h"
#include "../../src/assistant/live_protocol.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <cstdarg>
#include <cstdio>
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <bcrypt.h>

void portENTER_CRITICAL(portMUX_TYPE *p) { p->lock(); }
void portEXIT_CRITICAL(portMUX_TYPE *p) { p->unlock(); }
uint64_t assistantMonotonicUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace AssistantHost {
std::mutex mutex;
std::vector<std::string> jsonSent, requests, hosts;
std::vector<uint8_t> playback;
std::string serialOutput;
std::atomic<size_t> failPlaybackAfter{SIZE_MAX}, pausePlaybackAfter{SIZE_MAX};
std::atomic<bool> playbackPaused{false};
std::atomic<unsigned> audioStackBytes{0};
std::atomic<unsigned> captureReads{0}, writes{0}, tasksAlive{0}, socketsAlive{0}, rateChanges{0};
std::atomic<bool> codecOn{false}, captureInRead{false}, clockRace{false}, callbackEntered{false}, callbackExit{false};
std::atomic<int> failTaskAt{0}, taskCount{0}, connectDelay{0}, allocFailAt{0}, allocCount{0};
std::atomic<bool> failRead{false}, failWrite{false}, wrongAccept{false}, networkUnavailable{false};
std::atomic<uint32_t> timeOffset{0};
std::string afterSetup, afterEnd, afterToolResponse, redirectUrl;
std::atomic<unsigned> gatewayDelay{0};
std::atomic<bool> gatewayBadJson{false};
std::atomic<bool> redirectForever{false};
std::atomic<bool> gatewayLoseFirst{false};
std::string gatewayBody;
std::atomic<bool> phoneRoute{false}, releaseResponseTail{false}, responseHeadRead{false}, fastTaskYield{false};
std::atomic<bool> earlyPlayback{true};
std::string delayedAfterEnd;
std::atomic<size_t> playbackWriteLimit{3};
std::atomic<unsigned> closeDelayMs{0};
std::atomic<bool> closingSocket{false}, writeDuringClose{false};
std::vector<int16_t> sessionPrefix, sessionTail;
std::atomic<unsigned> capturePreparations{0}, playbackPreparations{0}, sessionReads{0}, sessionEnds{0};
std::atomic<unsigned> sessionStopRequests{0};
std::atomic<bool> sessionProducerAlive{false}, sessionStopRequested{false}, sessionReadAfterEnd{false};
std::atomic<bool> sessionFail{false}, sessionOverflow{false}, sessionRejectRate{false}, failPrepareCapture{false}, failPreparePlayback{false};
std::atomic<bool> playbackBeforeSessionEnd{false};
std::atomic<uint32_t> sessionOutputRate{0};
std::atomic<float> playbackVolume{0};
std::string wireAfterSetup, wireAfterEnd, wireAfterToolResponse;
std::atomic<bool> eofAfterSetup{false}, eofAfterEnd{false}, eofAfterToolResponse{false};
std::atomic<bool> wireTailRead{false};
void sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void reset() {
  assert(tasksAlive == 0 && socketsAlive == 0 && !sessionProducerAlive);
  std::lock_guard<std::mutex> lock(mutex);
  jsonSent.clear(); requests.clear(); hosts.clear(); playback.clear();
  serialOutput.clear(); audioStackBytes = 0;
  failPlaybackAfter = pausePlaybackAfter = SIZE_MAX; playbackPaused = false;
  captureReads = writes = rateChanges = taskCount = allocCount = 0;
  failTaskAt = connectDelay = allocFailAt = 0;
  codecOn = captureInRead = clockRace = callbackEntered = callbackExit = false;
  failRead = failWrite = wrongAccept = networkUnavailable = gatewayBadJson = redirectForever = gatewayLoseFirst = false;
  gatewayBody.clear();
  timeOffset = 0; gatewayDelay = 0; redirectUrl.clear(); afterSetup.clear(); afterToolResponse.clear();
  phoneRoute = releaseResponseTail = responseHeadRead = fastTaskYield = false;
  earlyPlayback = true;
  delayedAfterEnd.clear(); playbackWriteLimit = 3; closeDelayMs = 0;
  closingSocket = writeDuringClose = false;
  sessionPrefix.clear(); sessionTail.clear();
  capturePreparations = playbackPreparations = sessionReads = sessionEnds = 0;
  sessionStopRequested = sessionReadAfterEnd = sessionFail = sessionOverflow = sessionRejectRate = false;
  failPrepareCapture = failPreparePlayback = playbackBeforeSessionEnd = false;
  sessionOutputRate = 0; playbackVolume = 0;
  sessionStopRequests = 0;
  wireAfterSetup.clear(); wireAfterEnd.clear(); wireAfterToolResponse.clear();
  eofAfterSetup = eofAfterEnd = eofAfterToolResponse = false;
  wireTailRead = false;
  const uint8_t pcm[] = {0x34, 0x12, 0xfe, 0xff, 0, 0x80, 0xff, 0x7f};
  afterEnd = "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"" +
    Assistant::base64(pcm, sizeof(pcm)) + "\"}}]},\"turnComplete\":true}}";
}
std::vector<std::string> messages() { std::lock_guard<std::mutex> lock(mutex); return jsonSent; }
bool connectCallback() { callbackEntered = true; sleep(connectDelay.load()); callbackExit = true; return !networkUnavailable; }
} // namespace AssistantHost
namespace NetworkRoute { bool phoneSelected() { return AssistantHost::phoneRoute.load(); } }
namespace NetworkRoute { bool diagnosticEarlyPlayback() { return AssistantHost::earlyPlayback.load(); } }
bool NetworkClient::isPhone() const { return AssistantHost::phoneRoute.load(); }

struct AssistantHostTask {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<eTaskState> state{eRunning};
  bool deleted = false;
};
static thread_local TaskHandle_t currentTask = nullptr;
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned, TaskHandle_t *out, int) {
  if (!strcmp(name, "as_audio")) AssistantHost::audioStackBytes = stack;
  int n = ++AssistantHost::taskCount;
  if (AssistantHost::failTaskAt == n) return 0;
  auto h = new AssistantHostTask; *out = h; ++AssistantHost::tasksAlive;
  h->thread = std::thread([=] { currentTask = h; fn(arg); --AssistantHost::tasksAlive; });
  return pdPASS;
}
eTaskState eTaskGetState(TaskHandle_t h) { return h->state.load(); }
void vTaskDelay(unsigned ms) {
  if (AssistantHost::fastTaskYield && ms == 1) std::this_thread::yield();
  else AssistantHost::sleep(ms ? ms : 1);
}
void vTaskSuspend(TaskHandle_t h) {
  if (!h) h = currentTask;
  std::unique_lock<std::mutex> lock(h->mutex);
  h->state = eSuspended;
  h->cv.wait(lock, [&] { return h->deleted; });
}
void vTaskDelete(TaskHandle_t h) {
  assert(h && h->state == eSuspended);
  { std::lock_guard<std::mutex> lock(h->mutex); h->deleted = true; }
  h->cv.notify_all(); h->thread.join(); delete h;
}
uint32_t millis() {
  static const auto start = std::chrono::steady_clock::now();
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()) + AssistantHost::timeOffset;
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
AssistantHostSerial Serial;
void AssistantHostSerial::printf(const char *format, ...) {
  char line[1024];
  va_list args; va_start(args, format);
  const int length = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  assert(length >= 0 && size_t(length) < sizeof(line));
  std::lock_guard<std::mutex> lock(AssistantHost::mutex);
  AssistantHost::serialOutput.append(line, size_t(length));
}
void esp_fill_random(void *ptr, size_t n) {
  // Deliberately deterministic adapter, not a test of the ESP hardware RNG.
  static std::atomic<unsigned> seq{1}; unsigned seed = seq++;
  auto out = static_cast<uint8_t *>(ptr); for (size_t i = 0; i < n; ++i) out[i] = uint8_t(seed + i * 13);
}
int mbedtls_sha1_ret(const unsigned char *p, size_t n, unsigned char *out) {
  // Real SHA-1 from Windows CNG. Firmware uses native mbedTLS, not this adapter.
  BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  NTSTATUS s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
  if (s < 0) return -1;
  s = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  if (s >= 0) s = BCryptHashData(hash, const_cast<PUCHAR>(p), static_cast<ULONG>(n), 0);
  if (s >= 0) s = BCryptFinishHash(hash, out, 20, 0);
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0); return s < 0 ? -1 : 0;
}
void *heap_caps_malloc(size_t n, unsigned) {
  if (++AssistantHost::allocCount == AssistantHost::allocFailAt) return nullptr;
  return malloc(n);
}
bool AudioCodec::setSampleRate(uint32_t rate) {
  if (AssistantHost::captureInRead) AssistantHost::clockRace = true;
  if (rate == 24000) ++AssistantHost::rateChanges;
  return rate == 8000 || rate == 16000 || rate == 24000 || rate == 32000 || rate == 44100 || rate == 48000;
}
bool AudioCodec::enable(bool on) {
  if (!on && AssistantHost::captureInRead) AssistantHost::clockRace = true;
  AssistantHost::codecOn = on; return true;
}
bool AudioCodec::prepareCapture(uint32_t rate, float gain) {
  ++AssistantHost::capturePreparations;
  return !AssistantHost::failPrepareCapture && setSampleRate(rate) && enable(true) && setMicGain(gain);
}
bool AudioCodec::preparePlayback(uint32_t rate, float percent, bool boundedSpeech) {
  assert(boundedSpeech); // These fixtures exercise the real, peak-bounded speech path.
  ++AssistantHost::playbackPreparations;
  AssistantHost::playbackVolume = percent;
  if (AssistantHost::sessionProducerAlive || (AssistantHost::sessionReads && !AssistantHost::sessionEnds))
    AssistantHost::playbackBeforeSessionEnd = true;
  return !AssistantHost::failPreparePlayback && setSampleRate(rate) && setVolume(percent) && enable(true);
}
float AudioCodec::volumePercent() const { return AssistantHost::playbackVolume.load(); }
bool AudioCodec::readRegister(uint8_t address, uint16_t &value) {
  if (address != 0x32 || !AssistantHost::codecOn) return false;
  value = 0xBA; return true;
}
void AudioCodec::end() { enable(false); }
bool AudioCodec::read(void *ptr, size_t capacity, size_t *n, uint32_t) {
  AssistantHost::captureInRead = true;
  AssistantHost::sleep(2);
  if (AssistantHost::failRead) { *n = 0; AssistantHost::captureInRead = false; return false; }
  // Left = 0x1234, right = 0x5678. Output writes split stereo frames below.
  static thread_local size_t offset = 0;
  const uint8_t pattern[] = {0x34, 0x12, 0x78, 0x56};
  *n = std::min(capacity, size_t(128)); auto bytes = static_cast<uint8_t *>(ptr);
  for (size_t i = 0; i < *n; ++i) bytes[i] = pattern[(offset++) & 3];
  ++AssistantHost::captureReads; AssistantHost::captureInRead = false; return true;
}
bool AudioCodec::write(const void *ptr, size_t n, size_t *written, uint32_t) {
  if (AssistantHost::closingSocket) AssistantHost::writeDuringClose = true;
  if (AssistantHost::captureInRead) AssistantHost::clockRace = true;
  if (AssistantHost::failWrite || AssistantHost::playback.size() >= AssistantHost::failPlaybackAfter) { *written = 0; return false; }
  if (AssistantHost::playback.size() >= AssistantHost::pausePlaybackAfter) {
    AssistantHost::playbackPaused = true; *written = 0; return true;
  }
  *written = std::min(n, AssistantHost::playbackWriteLimit.load()); // Also exercise partial stereo frames.
  const auto *bytes = static_cast<const uint8_t *>(ptr);
  std::lock_guard<std::mutex> lock(AssistantHost::mutex);
  AssistantHost::playback.insert(AssistantHost::playback.end(), bytes, bytes + *written);
  ++AssistantHost::writes; return true;
}

bool CaptureSession::start(AudioCodec &codec, uint32_t rate, float gain) {
  if (!ended_.load() || !codec.prepareCapture(rate, gain)) return false;
  codec_ = &codec; inputRate_ = rate; ended_ = false; finished_ = false; stop_ = false;
  { std::lock_guard<std::mutex> lock(mutex_); pcm_ = AssistantHost::sessionPrefix; cursor_ = 0; }
  readStarted_ = outputReady_ = false;
  AssistantHost::sessionProducerAlive = true;
  producer_ = std::thread([this] {
    while (!stop_.load()) AssistantHost::sleep(1);
    // Model a read already in flight at release, followed by a converter tail.
    AssistantHost::sleep(20);
    { std::lock_guard<std::mutex> lock(mutex_);
      pcm_.insert(pcm_.end(), AssistantHost::sessionTail.begin(), AssistantHost::sessionTail.end()); }
    finished_.store(true, std::memory_order_release);
    AssistantHost::sessionProducerAlive = false;
  });
  return true;
}
bool CaptureSession::setOutputRate(uint32_t rate) {
  if (ended_.load() || readStarted_ || rate != 16000 || AssistantHost::sessionRejectRate) return false;
  AssistantHost::sessionOutputRate = rate; outputReady_ = true; return true;
}
bool CaptureSession::read(int16_t *mono, size_t capacity, size_t *samples, uint32_t timeoutMs) {
  *samples = 0;
  if (ended_.load()) { AssistantHost::sessionReadAfterEnd = true; return false; }
  if (!outputReady_ || !capacity) return false;
  readStarted_ = true; ++AssistantHost::sessionReads;
  const auto begin = std::chrono::steady_clock::now();
  do {
    { std::lock_guard<std::mutex> lock(mutex_);
      const size_t take = std::min(std::min(capacity, size_t(127)), pcm_.size() - cursor_);
      if (take) { memcpy(mono, pcm_.data() + cursor_, take * 2); cursor_ += take; *samples = take; return true; } }
    if (finished_.load()) return !failed();
    AssistantHost::sleep(1);
  } while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(timeoutMs));
  return !failed();
}
void CaptureSession::requestStop() {
  ++AssistantHost::sessionStopRequests;
  stop_ = true; AssistantHost::sessionStopRequested = true;
}
bool CaptureSession::drained() const {
  std::lock_guard<std::mutex> lock(mutex_); return finished_.load() && cursor_ == pcm_.size();
}
bool CaptureSession::failed() const { return AssistantHost::sessionFail.load(); }
uint32_t CaptureSession::overflowCount() const { return AssistantHost::sessionOverflow ? 1 : 0; }
void CaptureSession::end() {
  if (ended_.exchange(true)) return;
  requestStop(); if (producer_.joinable()) producer_.join();
  codec_->end(); ++AssistantHost::sessionEnds;
}

struct NetworkClient::Wire {
  bool live = false, connected = true, handshake = false, requestComplete = false;
  bool delayedResponse = false, eofWhenDrained = false;
  std::string outgoing, input;
  size_t received = 0;
};
static void serverFrame(NetworkClient::Wire &w, const std::string &json) {
  size_t separator = json.find('\x1e');
  if (separator != std::string::npos) {
    serverFrame(w, json.substr(0, separator)); serverFrame(w, json.substr(separator + 1)); return;
  }
  w.input += char(0x81);
  if (json.size() < 126) w.input += char(json.size());
  else if (json.size() <= 65535) { w.input += char(126); w.input += char(json.size() >> 8); w.input += char(json.size()); }
  else { w.input += char(127); for (int i = 7; i >= 0; --i) w.input += char(uint64_t(json.size()) >> (i * 8)); }
  w.input += json;
}
int NetworkClient::connect(const char *host, uint16_t port, int32_t timeout) {
  assert(port == 443 && timeout > 0); stop();
  { std::lock_guard<std::mutex> lock(AssistantHost::mutex); AssistantHost::hosts.emplace_back(host); }
  if (AssistantHost::networkUnavailable) return 0;
  wire = std::make_shared<Wire>(); ++AssistantHost::socketsAlive;
  wire->live = std::string(host) == "generativelanguage.googleapis.com"; return 1;
}
size_t NetworkClient::write(const uint8_t *p, size_t n) {
  if (!wire || !wire->connected) return 0;
  n = std::min(n, size_t(73)); wire->outgoing.append(reinterpret_cast<const char *>(p), n);
  auto &w = *wire;
  if (!w.live) return n;
  if (!w.handshake) {
    size_t end = w.outgoing.find("\r\n\r\n"); if (end == std::string::npos) return n;
    std::string name = "Sec-WebSocket-Key: "; size_t at = w.outgoing.find(name); assert(at != std::string::npos);
    at += name.size(); size_t e = w.outgoing.find("\r\n", at);
    std::string challenge = w.outgoing.substr(at, e - at) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20]; assert(!mbedtls_sha1_ret(reinterpret_cast<const unsigned char *>(challenge.data()), challenge.size(), digest));
    w.input = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
    w.input += AssistantHost::wrongAccept ? "wrong" : Assistant::base64(digest, 20);
    w.input += "\r\n\r\n";
    { std::lock_guard<std::mutex> lock(AssistantHost::mutex); AssistantHost::requests.push_back(w.outgoing); }
    w.outgoing.erase(0, end + 4); w.handshake = true;
  }
  while (w.outgoing.size() >= 2) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(w.outgoing.data());
    assert((bytes[0] & 0x80) && (bytes[1] & 0x80));
    size_t len = bytes[1] & 127, at = 2;
    if (len == 126) { if (w.outgoing.size() < 4) break; len = (size_t(bytes[2]) << 8) | bytes[3]; at = 4; }
    else if (len == 127) { if (w.outgoing.size() < 10) break; len = 0; for (int i = 2; i < 10; ++i) len = (len << 8) | bytes[i]; at = 10; }
    if (w.outgoing.size() < at + 4 + len) break;
    std::string payload = w.outgoing.substr(at + 4, len);
    for (size_t i = 0; i < len; ++i) payload[i] ^= bytes[at + (i & 3)];
    if ((bytes[0] & 15) == 1) {
      { std::lock_guard<std::mutex> lock(AssistantHost::mutex); AssistantHost::jsonSent.push_back(payload); }
      if (payload.find("\"setup\"") != std::string::npos) {
        serverFrame(w, "{\"setupComplete\":{}}");
        if (!AssistantHost::afterSetup.empty()) serverFrame(w, AssistantHost::afterSetup);
        w.input += AssistantHost::wireAfterSetup;
        w.eofWhenDrained = AssistantHost::eofAfterSetup;
      }
      if (payload.find("\"activityEnd\"") != std::string::npos) {
        if (!AssistantHost::afterEnd.empty()) serverFrame(w, AssistantHost::afterEnd);
        w.delayedResponse = !AssistantHost::delayedAfterEnd.empty();
        w.input += AssistantHost::wireAfterEnd;
        w.eofWhenDrained = AssistantHost::eofAfterEnd;
      }
      if (payload.find("\"toolResponse\"") != std::string::npos) {
        if (!AssistantHost::afterToolResponse.empty()) serverFrame(w, AssistantHost::afterToolResponse);
        w.input += AssistantHost::wireAfterToolResponse;
        w.eofWhenDrained = AssistantHost::eofAfterToolResponse;
      }
    }
    w.outgoing.erase(0, at + 4 + len);
  }
  return n;
}
int NetworkClient::read() {
  if (!available()) return -1;
  const int value = uint8_t(wire->input[wire->received++]);
  if (wire->received == wire->input.size() &&
      (!AssistantHost::wireAfterSetup.empty() || !AssistantHost::wireAfterEnd.empty() || !AssistantHost::wireAfterToolResponse.empty()))
    AssistantHost::wireTailRead = true;
  if (wire->delayedResponse && wire->received == wire->input.size()) AssistantHost::responseHeadRead = true;
  return value;
}
int NetworkClient::available() {
  if (!wire || !wire->connected) return 0;
  if (wire->delayedResponse && AssistantHost::releaseResponseTail.load()) {
    serverFrame(*wire, AssistantHost::delayedAfterEnd); wire->delayedResponse = false;
  }
  return int(wire->input.size() - wire->received);
}
bool NetworkClient::connected() {
  return wire && wire->connected && !(wire->eofWhenDrained && wire->received == wire->input.size());
}
void NetworkClient::stop() {
  if (wire && wire->connected) {
    if (AssistantHost::closeDelayMs) {
      AssistantHost::closingSocket = true; AssistantHost::sleep(AssistantHost::closeDelayMs);
      AssistantHost::closingSocket = false;
    }
    wire->connected = false; --AssistantHost::socketsAlive;
  }
  wire.reset();
}
bool HttpClient::readResponse(NetworkClient &client, HttpResponse &out, size_t limit, uint32_t timeout) {
  assert(limit == 32768 && timeout <= 8000); assert(client.wire);
  AssistantHost::sleep(AssistantHost::gatewayDelay);
  const std::string request = client.wire->outgoing;
  { std::lock_guard<std::mutex> lock(AssistantHost::mutex); AssistantHost::requests.push_back(request); }
  if (AssistantHost::gatewayLoseFirst.exchange(false)) return false;
  out.headersComplete = out.bodyComplete = true;
  if ((request.compare(0, 5, "POST ") == 0 || AssistantHost::redirectForever) && !AssistantHost::redirectUrl.empty()) {
    out.statusCode = 302; out.location = AssistantHost::redirectUrl; return true;
  }
  out.statusCode = 200;
  if (!AssistantHost::gatewayBody.empty()) { out.body = AssistantHost::gatewayBody; return true; }
  if (AssistantHost::gatewayBadJson) { out.body = "<html>bad</html>"; return true; }
  size_t begin = request.find("\r\n\r\n"); assert(begin != std::string::npos);
  std::string id;
  if (request.compare(0, 5, "POST ") == 0) {
    auto json = Assistant::parseJson(request.data() + begin + 4, request.size() - begin - 4);
    assert(json); id = Assistant::stringField(json.get(), "id");
  } else id = Assistant::gatewayId(1, 0);
  out.body = "{\"version\":1,\"id\":\"" + id + "\",\"ok\":true,\"result\":{\"saved\":true}}";
  return true;
}
