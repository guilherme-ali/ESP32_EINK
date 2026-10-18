#include "../../src/assistant/phone_voice.h"
#include "../../src/assistant/live_protocol.h"
#include "../../src/connectivity/ble_relay.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>
using namespace Assistant;
static unsigned checks, clockMs, readCount, resets;
static bool linked;
static std::deque<uint8_t> incoming;
static std::vector<uint8_t> outgoing;
static std::vector<Media::Header> sent;
static std::vector<std::vector<uint8_t>> sentBodies;
static std::vector<uint8_t> written;
static std::vector<size_t> writeSizes;
static size_t packetBytes = 4, shortWriteAt, shortBytes;
static unsigned encodeCalls, cancelAfterEncode, closeAfterEncode;
static bool canceled;
static PhoneVoice *encodingVoice;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
uint32_t millis() { return clockMs; }
uint64_t assistantMonotonicUs() { return uint64_t(clockMs) * 1000; }
void vTaskDelay(unsigned ticks) { clockMs += ticks; }
VoiceOpus::~VoiceOpus() {}
bool VoiceOpus::begin(int) { lookahead_ = 104; return true; }
int VoiceOpus::encoderLookahead() const { return lookahead_; }
void VoiceOpus::resetDecoder() { ++resets; }
int VoiceOpus::encode(const int16_t *, size_t n, uint8_t *out, size_t cap) {
  CHECK(n == 320 && cap >= packetBytes);
  ++encodeCalls;
  for (size_t i = 0; i < packetBytes; ++i) out[i] = uint8_t(17 + encodeCalls + i);
  if (encodeCalls == cancelAfterEncode) canceled = true;
  if (encodeCalls == closeAfterEncode && encodingVoice) encodingVoice->close();
  return static_cast<int>(packetBytes);
}
int VoiceOpus::decode(const uint8_t *, size_t, int16_t *out, size_t n) {
  CHECK(n == 480); for (size_t i = 0; i < n; ++i) out[i] = static_cast<int16_t>(i); return 480;
}
static void enqueue(Media::Type type, const std::vector<uint8_t> &body, uint32_t position = 0, uint8_t flags = 0) {
  Media::Header h; h.type = type; h.length = body.size(); h.position = position; h.flags = flags;
  uint8_t header[Media::kHeaderSize]; Media::encodeHeader(h, header);
  incoming.insert(incoming.end(), header, header + sizeof(header)); incoming.insert(incoming.end(), body.begin(), body.end());
}
static std::vector<uint8_t> text(const std::string &s) { return {s.begin(), s.end()}; }
namespace PhoneRelay {
int open(const char *host, uint16_t port, uint32_t) {
  CHECK(!strcmp(host, Media::kEndpoint) && port == 443); linked = true; return 0;
}
void close(int) { linked = false; }
bool connected(int) { return linked; }
int available(int) { return incoming.size(); }
int read(int, uint8_t *bytes, size_t n) {
  if (!n) return 0;
  // Match the real API, including normal -1 pauses inside header/payload.
  if (++readCount % 3 == 0 || incoming.empty()) return -1;
  size_t take = std::min({n, incoming.size(), size_t(3)});
  for (size_t i = 0; i < take; ++i) { bytes[i] = incoming.front(); incoming.pop_front(); }
  return static_cast<int>(take);
}
size_t write(int, const uint8_t *p, size_t n, uint32_t) {
  writeSizes.push_back(n);
  const size_t done = writeSizes.size() == shortWriteAt ? std::min(n, shortBytes) : n;
  written.insert(written.end(), p, p + done);
  outgoing.insert(outgoing.end(), p, p + done);
  while (outgoing.size() >= Media::kHeaderSize) {
    Media::Header h; CHECK(Media::decodeHeader(outgoing.data(), h));
    if (outgoing.size() < Media::kHeaderSize + h.length) break;
    const std::vector<uint8_t> body(outgoing.begin() + Media::kHeaderSize, outgoing.begin() + Media::kHeaderSize + h.length);
    sent.push_back(h); sentBodies.push_back(body);
    if (h.type == Media::Type::Hello) {
      auto json = parseJson(reinterpret_cast<const char *>(body.data()), body.size()); CHECK(bool(json));
      const int bitrate = field(json.get(), "bitrate")->valueint;
      std::string hello = "{\"version\":1,\"codec\":\"" + std::string(bitrate ? "opus" : "pcm") +
        "\",\"bitrate\":" + std::to_string(bitrate) + ",\"inputRate\":16000,\"outputRate\":24000,\"frameMs\":20,\"outputLookahead\":" +
        std::string(bitrate ? "156" : "0") + "}";
      enqueue(Media::Type::Hello, text(hello));
    }
    outgoing.erase(outgoing.begin(), outgoing.begin() + Media::kHeaderSize + h.length);
  }
  return done;
}
}
static WsParser::Event wait(PhoneVoice &voice) {
  for (unsigned i = 0; i < 10000; ++i) {
    const auto e = voice.poll(); if (e != WsParser::Event::None) return e; vTaskDelay(1);
  }
  CHECK(false); return WsParser::Event::Error;
}
static bool stop(void *) { return canceled; }
static void reset() {
  incoming.clear(); outgoing.clear(); sent.clear(); sentBodies.clear(); written.clear(); writeSizes.clear();
  readCount = clockMs = resets = encodeCalls = cancelAfterEncode = closeAfterEncode = 0;
  packetBytes = 4; shortWriteAt = shortBytes = 0; canceled = false; encodingVoice = nullptr;
}
static std::vector<uint8_t> upload(std::vector<uint8_t> &arena, size_t packet, size_t block, unsigned total) {
  reset(); packetBytes = packet;
  PhoneVoice voice(arena.data() + 1, Media::kJsonLimit + 1, stop, nullptr);
  CHECK(voice.connect("not-a-secret-test-key", 48000));
  CHECK(voice.sendJson("{\"before\":true}"));
  std::vector<uint8_t> pcm(total * 2, 123);
  for (size_t at = 0; at < pcm.size();) {
    const size_t n = std::min(block, pcm.size() - at);
    const unsigned before = encodeCalls, time = clockMs;
    const size_t records = sent.size();
    CHECK(voice.sendAudio(pcm.data() + at, n));
    CHECK(clockMs == time); // No timer/delay and no complete record retained across calls.
    CHECK(sent.size() - records == encodeCalls - before);
    CHECK(outgoing.empty());
    at += n;
  }
  CHECK(voice.sendJson("{\"after\":true}"));
  CHECK(sent.back().type == Media::Type::Json);
  CHECK(voice.endInput());
  CHECK(outgoing.empty() && sent.back().type == Media::Type::EndInput);
  CHECK(sent.back().position == total && voice.sentBytes() == written.size());
  CHECK(arena.front() == 0xA5 && arena.back() == 0xA5);
  return written;
}
int main() {
  // Canary surrounds the receive arena to expose sign/offset errors.
  std::vector<uint8_t> arena(Media::kJsonLimit + 3, 0xA5);
  for (unsigned total : {1u, 319u, 320u, 321u, 640u, 641u, 16001u}) {
    reset(); PhoneVoice voice(arena.data() + 1, Media::kJsonLimit + 1, stop, nullptr);
    CHECK(voice.connect("not-a-secret-test-key", 64000));
    CHECK(voice.poll() == WsParser::Event::None); // -1 while waiting is not EOF or a giant size_t.
    std::vector<uint8_t> pcm(total * 2, 123);
    for (size_t at = 0; at < pcm.size();) {
      const size_t n = std::min(size_t(2048), pcm.size() - at);
      CHECK(voice.sendAudio(pcm.data() + at, n)); at += n;
    }
    CHECK(voice.endInput());
    unsigned emitted = 0;
    for (size_t i = 0; i < sent.size(); ++i) if (sent[i].type == Media::Type::Opus) {
      CHECK(sent[i].position == emitted);
      const auto &b = sentBodies[i]; const unsigned take = unsigned(b[2]) | unsigned(b[3]) << 8;
      emitted += take;
    }
    CHECK(emitted == total && sent.back().type == Media::Type::EndInput && sent.back().position == total);
    enqueue(Media::Type::Opus, {156,0,68,1,17}, 0, 1); // 324 original samples after pre-skip.
    CHECK(wait(voice) == WsParser::Event::Audio && voice.size() == 648);
    const int16_t *decoded = reinterpret_cast<const int16_t *>(voice.data());
    CHECK(decoded[0] == 156 && decoded[323] == 479 && resets == 1);
    enqueue(Media::Type::Opus, {0,0,7,0,17}, 324);
    CHECK(wait(voice) == WsParser::Event::Audio && voice.size() == 14);
    enqueue(Media::Type::Json, text("{\"serverContent\":{\"turnComplete\":true}}"));
    CHECK(wait(voice) == WsParser::Event::Message);
    enqueue(Media::Type::Opus, {156,0,1,0,17}, 331, 1);
    CHECK(wait(voice) == WsParser::Event::Audio && voice.size() == 2 && resets == 2);
    enqueue(Media::Type::Opus, {0,0,1,0,17}, 999);
    CHECK(wait(voice) == WsParser::Event::Error); // no PLC, drop, replay, or out-of-order acceptance.
    CHECK(arena.front() == 0xA5 && arena.back() == 0xA5);
  }
  reset();
  {
    PhoneVoice voice(arena.data() + 1, Media::kJsonLimit + 1, stop, nullptr);
    CHECK(voice.connect("not-a-secret-test-key", 0));
    const uint8_t pcm[] = {1,2,3,4}; CHECK(voice.sendAudio(pcm, sizeof(pcm))); CHECK(voice.endInput());
    enqueue(Media::Type::Pcm, {1,2,3,4});
    CHECK(wait(voice) == WsParser::Event::Audio && voice.size() == 4 && !memcmp(voice.data(), pcm, sizeof(pcm)));
    linked = false; CHECK(wait(voice) == WsParser::Event::Error);
    CHECK(arena.front() == 0xA5 && arena.back() == 0xA5);
  }
  // A frame-per-call reference preserves the original write granularity and wire bytes.
  for (size_t packet : {size_t(4), size_t(120), size_t(160), size_t(173), size_t(676), Media::kPacketLimit}) {
    for (unsigned total : {1u, 319u, 320u, 321u, 5120u, 5121u, 16001u}) {
      const auto reference = upload(arena, packet, 640, total);
      for (size_t block : {size_t(638), size_t(2048), size_t(8192), size_t(10240)}) {
        CHECK(upload(arena, packet, block, total) == reference);
        for (size_t n : writeSizes) CHECK(n <= 1384);
      }
    }
  }
  for (size_t packet : {size_t(120), size_t(160), size_t(676)}) {
    const auto reference = upload(arena, packet, 640, 5120);
    const size_t separate = writeSizes.size();
    CHECK(upload(arena, packet, 16 * 640, 5120) == reference);
    CHECK(writeSizes.size() < separate);
    if (packet == 676) CHECK(std::count(writeSizes.begin(), writeSizes.end(), size_t(1384)) == 8);
  }
  // Closing/canceling with one staged record must never flush it, even in the destructor.
  for (bool cancel : {false, true}) {
    reset(); packetBytes = 120;
    {
      PhoneVoice voice(arena.data() + 1, Media::kJsonLimit + 1, stop, nullptr);
      CHECK(voice.connect("not-a-secret-test-key", 48000));
      encodingVoice = &voice;
      if (cancel) cancelAfterEncode = 2; else closeAfterEncode = 2;
      const auto prefix = written;
      const size_t calls = writeSizes.size();
      std::vector<uint8_t> pcm(16 * 640, 123);
      CHECK(!voice.sendAudio(pcm.data(), pcm.size()));
      CHECK(!linked && written == prefix && writeSizes.size() == calls);
      canceled = false;
      CHECK(!voice.sendJson("{}"));
      voice.close();
      CHECK(written == prefix && writeSizes.size() == calls);
    }
    CHECK(writeSizes.size() == 1); // HELLO only; destructor did not send pending audio/tail.
  }
  // A short batch write accepts only a prefix and closes without retrying any bytes.
  for (size_t accepted : {size_t(0), size_t(1), size_t(175), size_t(1231)}) {
    reset(); packetBytes = 160;
    PhoneVoice voice(arena.data() + 1, Media::kJsonLimit + 1, stop, nullptr);
    CHECK(voice.connect("not-a-secret-test-key", 48000));
    shortWriteAt = writeSizes.size() + 1; shortBytes = accepted;
    const size_t prefix = written.size();
    std::vector<uint8_t> pcm(16 * 640, 123);
    CHECK(!voice.sendAudio(pcm.data(), pcm.size()));
    CHECK(!linked && writeSizes.back() == 1232 && written.size() == prefix + accepted);
    CHECK(voice.sentBytes() == prefix + accepted);
    CHECK(strstr(voice.error(), "sem replay") != nullptr);
    const auto failed = written;
    const size_t calls = writeSizes.size();
    CHECK(!voice.endInput()); CHECK(!voice.sendJson("{}")); voice.close();
    CHECK(written == failed && writeSizes.size() == calls);
  }
  // Failure while flushing the padded endInput tail must not emit END_INPUT.
  reset();
  {
    PhoneVoice voice(arena.data() + 1, Media::kJsonLimit + 1, stop, nullptr);
    CHECK(voice.connect("not-a-secret-test-key", 48000));
    const uint8_t sample[] = {1, 2};
    CHECK(voice.sendAudio(sample, sizeof(sample)) && encodeCalls == 0);
    shortWriteAt = writeSizes.size() + 1; shortBytes = 1;
    CHECK(!voice.endInput() && !linked);
    CHECK(sent.back().type == Media::Type::Hello && writeSizes.size() == 2);
  }
  std::cout << "PhoneVoice: " << checks << " checks (stream bytes, batching, tails, cancel, short writes, profiles, canaries) OK\n";
}
