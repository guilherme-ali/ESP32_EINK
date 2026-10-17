#include "audio/voice_opus.h"
#include <esp_heap_caps.h>
#include <opus.h>
#include <assert.h>
#include <chrono>
#include <initializer_list>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

static bool hot = false;
static size_t hotAllocations = 0;
static size_t heapCalls = 0;
static size_t heapLive = 0;
static size_t heapBytes = 0;
static int heapBudget = -1;
static bool noPsram = false;
static size_t internalAllocations = 0;
static void *states[8] = {};
static size_t sizes[8] = {};

extern "C" void *__real_malloc(size_t bytes);
extern "C" void *__real_calloc(size_t n, size_t bytes);
extern "C" void *__real_realloc(void *p, size_t bytes);
extern "C" void __real_free(void *p);
extern "C" void *__wrap_malloc(size_t bytes) {
  if (hot) ++hotAllocations;
  return __real_malloc(bytes);
}
extern "C" void *__wrap_calloc(size_t n, size_t bytes) {
  if (hot) ++hotAllocations;
  return __real_calloc(n, bytes);
}
extern "C" void *__wrap_realloc(void *p, size_t bytes) {
  if (hot) ++hotAllocations;
  return __real_realloc(p, bytes);
}
extern "C" void __wrap_free(void *p) {
  __real_free(p);
}

extern "C" void *heap_caps_malloc(size_t bytes, uint32_t caps) {
  ++heapCalls;
  if (hot) ++hotAllocations;
  if ((noPsram && (caps & MALLOC_CAP_SPIRAM)) || heapBudget == 0) return nullptr;
  if (heapBudget > 0) --heapBudget;
  assert((caps & MALLOC_CAP_8BIT) != 0);
  void *p = __real_malloc(bytes);
  assert(p);
  for (size_t i = 0; i < 8; ++i) {
    if (!states[i]) {
      states[i] = p;
      sizes[i] = bytes;
      ++heapLive;
      heapBytes += bytes;
      if (caps & MALLOC_CAP_INTERNAL) ++internalAllocations;
      return p;
    }
  }
  abort();
}

extern "C" void heap_caps_free(void *p) {
  if (!p) return;
  for (size_t i = 0; i < 8; ++i) {
    if (states[i] == p) {
      states[i] = nullptr;
      --heapLive;
      heapBytes -= sizes[i];
      __real_free(p);
      return;
    }
  }
  abort(); // Unknown allocation or double-free.
}

static void signal(int16_t *out, size_t n, int offset = 0) {
  for (size_t i = 0; i < n; ++i) {
    const int phase = (static_cast<int>(i) + offset) % 80;
    out[i] = static_cast<int16_t>((phase < 40 ? phase : 80 - phase) * 600 - 12000);
  }
}

static void cold() {
  VoiceOpus codec;
  int16_t pcm[320] = {};
  uint8_t packet[1276] = {};
  assert(codec.encoderLookahead() == 0);
  assert(codec.encode(pcm, 320, packet, sizeof(packet)) == OPUS_INVALID_STATE);
  assert(codec.decode(packet, 1, pcm, 320) == OPUS_INVALID_STATE);
  codec.reset();
  assert(strcmp(codec.error(), opus_strerror(OPUS_INVALID_STATE)) == 0);
  assert(!codec.begin(0));
  assert(!codec.begin(499));
  assert(!codec.begin(512001));
  assert(heapLive == 0);
}

static void contract() {
  VoiceOpus codec;
  assert(codec.begin());
  assert(codec.encoderLookahead() > 0 && codec.encoderLookahead() <= 320);
  int16_t pcm[320];
  signal(pcm, 320);
  uint8_t packet[1278];
  memset(packet, 0xA5, sizeof(packet));
  assert(codec.encode(nullptr, 320, packet, 1276) == OPUS_BAD_ARG);
  assert(codec.encode(pcm, 319, packet, 1276) == OPUS_BAD_ARG);
  assert(codec.encode(pcm, 321, packet, 1276) == OPUS_BAD_ARG);
  assert(codec.encode(pcm, 320, nullptr, 1276) == OPUS_BAD_ARG);
  assert(codec.encode(pcm, 320, packet, 0) == OPUS_BUFFER_TOO_SMALL);
  const int bytes = codec.encode(pcm, 320, packet + 1, SIZE_MAX);
  assert(bytes > 0 && bytes <= 1276);
  assert(packet[0] == 0xA5 && packet[1277] == 0xA5);
  assert(opus_packet_get_nb_samples(packet + 1, bytes, 16000) == 320);
  assert(opus_packet_get_nb_channels(packet + 1) == 1);
  int16_t output[482];
  for (auto &sample : output) sample = 12345;
  assert(codec.decode(packet + 1, bytes, output + 1, 479) == OPUS_BUFFER_TOO_SMALL);
  assert(output[1] == 12345);
  assert(codec.decode(nullptr, 0, output + 1, 480) == OPUS_BAD_ARG);
  assert(codec.decode(packet + 1, 0, output + 1, 480) == OPUS_BAD_ARG);
  assert(codec.decode(packet + 1, bytes, nullptr, 480) == OPUS_BAD_ARG);
  assert(codec.decode(packet + 1, static_cast<size_t>(INT32_MAX) + 1, output, 480) == OPUS_BAD_ARG);
  assert(codec.decode(packet + 1, bytes, output + 1, SIZE_MAX) == 480);
  assert(output[0] == 12345 && output[481] == 12345);
  assert(strcmp(codec.error(), opus_strerror(OPUS_OK)) == 0);
  const uint8_t malformed[] = {0x9B, 0x00};
  assert(codec.decode(malformed, sizeof(malformed), output, 480) == OPUS_INVALID_PACKET);
}

static void interoperability() {
  VoiceOpus codec;
  assert(codec.begin());
  int error = OPUS_OK;
  OpusEncoder *encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &error);
  assert(encoder && error == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000)) == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5)) == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(1)) == OPUS_OK);
  OpusDecoder *decoder = opus_decoder_create(24000, 1, &error);
  assert(decoder && error == OPUS_OK);
  int16_t pcm[320], actual[480], expected[480];
  uint8_t packet[1276], reference[1276];
  hot = true;
  for (int frame = 0; frame < 100; ++frame) {
    signal(pcm, 320, frame * 320);
    const int bytes = codec.encode(pcm, 320, packet, sizeof(packet));
    const int refBytes = opus_encode(encoder, pcm, 320, reference, sizeof(reference));
    assert(bytes > 0 && bytes == refBytes);
    assert(memcmp(packet, reference, bytes) == 0);
    assert(codec.decode(reference, refBytes, actual, 480) == 480);
    assert(opus_decode(decoder, packet, bytes, expected, 480, 0) == 480);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
  }
  hot = false;
  opus_encoder_destroy(encoder);
  opus_decoder_destroy(decoder);
}

static void downlinkAndRejections() {
  VoiceOpus codec;
  assert(codec.begin());
  int error = OPUS_OK;
  OpusEncoder *encoder = opus_encoder_create(24000, 1, OPUS_APPLICATION_AUDIO, &error);
  assert(encoder && error == OPUS_OK);
  int16_t pcm[2880], out[480];
  signal(pcm, 2880);
  uint8_t packet[4000];
  for (const int samples : {60, 120, 240, 480, 960, 1440}) {
    const int bytes = opus_encode(encoder, pcm, samples, packet, sizeof(packet));
    assert(bytes > 0);
    const int result = codec.decode(packet, bytes, out, 480);
    assert(result == (samples == 480 ? 480 : OPUS_INVALID_PACKET));
  }
  opus_encoder_destroy(encoder);
  encoder = opus_encoder_create(24000, 2, OPUS_APPLICATION_AUDIO, &error);
  assert(encoder && error == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(2)) == OPUS_OK);
  const int bytes = opus_encode(encoder, pcm, 480, packet, sizeof(packet));
  assert(bytes > 0 && opus_packet_get_nb_channels(packet) == 2);
  assert(codec.decode(packet, bytes, out, 480) == OPUS_INVALID_PACKET);
  opus_encoder_destroy(encoder);
}

static void resetAndReuse() {
  VoiceOpus codec;
  assert(codec.begin());
  int16_t pcm[320], first[480], replay[480];
  signal(pcm, 320);
  uint8_t packet[1276], again[1276];
  const int bytes = codec.encode(pcm, 320, packet, sizeof(packet));
  assert(bytes > 0 && codec.decode(packet, bytes, first, 480) == 480);
  const size_t calls = heapCalls;
  const int lookahead = codec.encoderLookahead();
  hot = true;
  for (int i = 0; i < 100; ++i) {
    codec.reset();
    assert(codec.encoderLookahead() == lookahead);
    const int next = codec.encode(pcm, 320, again, sizeof(again));
    assert(next == bytes && memcmp(packet, again, bytes) == 0);
    assert(codec.decode(again, next, replay, 480) == 480);
    assert(memcmp(first, replay, sizeof(first)) == 0);
  }
  assert(codec.begin(32000));
  assert(!codec.begin(-1)); // Bad configuration does not discard a good state.
  assert(codec.encode(pcm, 320, packet, sizeof(packet)) > 0);
  hot = false;
  assert(heapCalls == calls);
}

static void allocationFailures() {
  assert(heapLive == 0);
  for (int budget = 0; budget <= 1; ++budget) {
    VoiceOpus codec;
    heapBudget = budget;
    assert(!codec.begin());
    assert(heapLive == 0 && heapBytes == 0);
    assert(codec.encoderLookahead() == 0);
    assert(strcmp(codec.error(), opus_strerror(OPUS_ALLOC_FAIL)) == 0);
    heapBudget = -1;
    assert(codec.begin());
  }
  assert(heapLive == 0);
  const size_t before = internalAllocations;
  noPsram = true;
  {
    VoiceOpus codec;
    assert(codec.begin());
    assert(heapLive == 2 && internalAllocations == before + 2);
  }
  noPsram = false;
  assert(heapLive == 0);
}

static void independentResets() {
  VoiceOpus actual, reference;
  assert(actual.begin() && reference.begin());
  int16_t pcm[320], out[480], expected[480];
  signal(pcm, 320);
  uint8_t packet[1276], refPacket[1276];
  hot = true;
  int bytes = actual.encode(pcm, 320, packet, sizeof(packet));
  int refBytes = reference.encode(pcm, 320, refPacket, sizeof(refPacket));
  assert(bytes > 0 && bytes == refBytes);
  assert(actual.decode(packet, bytes, out, 480) == 480);
  assert(reference.decode(packet, bytes, expected, 480) == 480);
  actual.resetDecoder();
  bytes = actual.encode(pcm, 320, packet, sizeof(packet));
  refBytes = reference.encode(pcm, 320, refPacket, sizeof(refPacket));
  assert(bytes > 0 && bytes == refBytes && memcmp(packet, refPacket, bytes) == 0);
  actual.resetDecoder();
  reference.resetDecoder();
  assert(actual.decode(packet, bytes, out, 480) == 480);
  assert(reference.decode(packet, bytes, expected, 480) == 480);
  actual.resetEncoder();
  assert(actual.decode(packet, bytes, out, 480) == 480);
  assert(reference.decode(packet, bytes, expected, 480) == 480);
  assert(memcmp(out, expected, sizeof(out)) == 0);
  hot = false;
}

static void benchmark() {
  VoiceOpus codec;
  assert(codec.begin());
  int16_t pcm[320], out[480];
  signal(pcm, 320);
  uint8_t packet[1276];
  size_t total = 0, largest = 0;
  long long encodeNs = 0, decodeNs = 0;
  for (int frame = 0; frame < 500; ++frame) {
    const auto start = std::chrono::steady_clock::now();
    hot = true;
    const int bytes = codec.encode(pcm, 320, packet, sizeof(packet));
    hot = false;
    const auto middle = std::chrono::steady_clock::now();
    assert(bytes > 0);
    hot = true;
    assert(codec.decode(packet, bytes, out, 480) == 480);
    hot = false;
    const auto end = std::chrono::steady_clock::now();
    encodeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(middle - start).count();
    decodeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(end - middle).count();
    total += bytes;
    if (static_cast<size_t>(bytes) > largest) largest = bytes;
  }
  printf("Opus state bytes: encoder=%d decoder=%d total=%zu; lookahead16k=%d\n",
         opus_encoder_get_size(1), opus_decoder_get_size(1), heapBytes, codec.encoderLookahead());
  printf("HOST ONLY, 500x20ms: encode=%.3fms/frame decode=%.3fms/frame bitrate=%.1fbps max=%zuB\n",
         encodeNs / 500e6, decodeNs / 500e6, total * 8.0 / 10.0, largest);
}

int main() {
  static_assert(!std::is_copy_constructible<VoiceOpus>::value, "State must not be copied");
  static_assert(!std::is_copy_assignable<VoiceOpus>::value, "State must not be copied");
  cold();
  contract();
  interoperability();
  downlinkAndRejections();
  resetAndReuse();
  allocationFailures();
  independentResets();
  benchmark();
  assert(hotAllocations == 0);
  assert(heapLive == 0 && heapBytes == 0);
  printf("VoiceOpus: 8 groups OK; no hot-path malloc/calloc/realloc; no leaked states\n");
}
