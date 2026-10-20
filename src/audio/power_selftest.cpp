#include "power_selftest.h"

#if ARDUINO_USB_CDC_ON_BOOT
#include "capture_session.h"
#include "recorder.h"
#include "wav.h"
#include "../storage/note_files.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cmath>
#include <cstdio>

namespace {
constexpr int64_t kDeadlineUs = 10000000;
struct RateCase { uint32_t input, output; };
constexpr RateCase kCases[] = {
    {16000, 8000}, {16000, 16000}, {32000, 32000}, {44100, 44100}, {48000, 48000},
    {32000, 16000}, {44100, 16000}, {48000, 16000}};

struct CaseResult {
  uint64_t setupUs = 0, durationUs = 0;
  uint32_t bytes = 0, samples = 0, overflow = 0, nonzero = 0, hash = 2166136261U;
  int64_t sum = 0;
  uint32_t peak = 0;
  bool started = false, recorded = false, wav = false, cleanup = false;
  bool failed = false, sessionFailed = false, storageFull = false, timeout = false;
  bool quiescent = false;
};

bool casePassed(const CaseResult &r) {
  return r.started && r.recorded && r.wav && r.cleanup && r.quiescent &&
         !r.failed && !r.sessionFailed && !r.storageFull && !r.timeout && !r.overflow;
}

bool readExact(File &reader, uint8_t *out, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const size_t got = reader.read(out + offset, length - offset);
    if (!got || got > length - offset) return false;
    offset += got;
  }
  return true;
}

// Recebe a visao virtual header + PCM append-only, nunca o .wav fisico de 44B.
// Le tudo, inclusive silencio, sem usar pico/hash como criterio de audio valido.
bool inspectWav(File &reader, uint32_t outputRate, CaseResult &r) {
  WavHeader header;
  if (!reader || reader.isDirectory() ||
      !readExact(reader, reinterpret_cast<uint8_t *>(&header), sizeof(header)) ||
      !validWavHeader(header, reader.size()) || header.sampleRate != outputRate ||
      header.numChannels != 1 || header.bitsPerSample != 16 || header.dataSize != r.bytes)
    return false;

  const int64_t deadline = esp_timer_get_time() + kDeadlineUs;
  uint8_t buffer[512];
  uint32_t remaining = header.dataSize;
  bool haveLowByte = false;
  uint8_t lowByte = 0;
  while (remaining) {
    if (esp_timer_get_time() >= deadline) return false;
    const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const size_t got = reader.read(buffer, wanted);
    if (!got || got > wanted) return false;
    for (size_t i = 0; i < got; ++i) {
      r.hash = (r.hash ^ buffer[i]) * 16777619U;
      if (!haveLowByte) lowByte = buffer[i];
      else {
        const uint32_t raw = lowByte | (static_cast<uint32_t>(buffer[i]) << 8);
        const int32_t value = raw < 32768 ? static_cast<int32_t>(raw) : static_cast<int32_t>(raw) - 65536;
        const uint32_t amplitude = static_cast<uint32_t>(value < 0 ? -value : value);
        if (amplitude > r.peak) r.peak = amplitude;
        if (value) ++r.nonzero;
        r.sum += value;
        ++r.samples;
      }
      haveLowByte = !haveLowByte;
    }
    remaining -= static_cast<uint32_t>(got);
    delay(1);
  }
  r.durationUs = static_cast<uint64_t>(r.samples) * 1000000 / outputRate;
  return !haveLowByte && r.samples == r.bytes / 2 && reader.position() == reader.size() &&
         reader.size() == sizeof(WavHeader) + r.bytes && reader.read() == -1;
}

bool removeOwnDirectory(const char *directory) {
  File dir = LittleFS.open(directory, FILE_READ);
  if (!dir || !dir.isDirectory()) { dir.close(); return false; }
  File child = dir.openNextFile();
  const bool empty = !child;
  child.close();
  dir.close();
  // Nunca remover recursivamente: um arquivo inesperado nao pertence ao teste.
  return empty && LittleFS.rmdir(directory) && !LittleFS.exists(directory);
}

void snapshotFailures(const CaptureSession &session, const Recorder &recorder, CaseResult &r) {
  r.sessionFailed = session.failed();
  r.overflow = session.overflowCount();
  if (r.recorded || recorder.isActive()) {
    r.failed = r.failed || recorder.writeFailed();
    r.storageFull = recorder.storageFull();
    if (recorder.overflowCount() > r.overflow) r.overflow = recorder.overflowCount();
    r.bytes = recorder.bytesWritten();
  }
}

bool stopAndJoin(CaptureSession &session, Recorder &recorder, CaseResult &r) {
  const int64_t deadline = esp_timer_get_time() + kDeadlineUs;
  session.requestStop();
  if (recorder.isActive()) {
    recorder.requestStop();
    while (!recorder.isFinished()) {
      if (esp_timer_get_time() >= deadline) {
        r.failed = r.timeout = true;
        snapshotFailures(session, recorder, r);
        return false; // File/ring/codec continuam pertencendo aos workers.
      }
      delay(5);
    }
    snapshotFailures(session, recorder, r);
    r.bytes = recorder.finish(); // end() da sessao somente apos writer drenar.
  } else if (session.active()) {
    // start do Recorder pode falhar ANTES de adotar a sessao. Ainda somos donos
    // do unico consumidor; drenar o prefixo/tail antes de encerrar o produtor.
    int16_t discard[256];
    while (!session.drained()) {
      if (esp_timer_get_time() >= deadline) {
        r.failed = r.timeout = true;
        snapshotFailures(session, recorder, r);
        return false;
      }
      size_t samples = 0;
      session.read(discard, sizeof(discard) / sizeof(discard[0]), &samples, 20);
      delay(1);
    }
    snapshotFailures(session, recorder, r);
    session.end();
  }
  return true;
}

void reportCase(uint32_t rep, size_t index, const char *path, const CaseResult &r) {
  Serial.printf(
      "[PowerTest] {\"rep\":%lu,\"case\":%u,\"in_hz\":%lu,\"out_hz\":%lu,"
      "\"setup_us\":%llu,\"captured_samples\":%lu,\"bytes\":%lu,\"duration_us\":%llu,"
      "\"overflow\":%lu,\"failed\":%s,\"session_failed\":%s,\"storage_full\":%s,"
      "\"cleanup\":%s,\"timeout\":%s,\"wav\":%s,\"quiescent\":%s,"
      "\"pcm_peak\":%lu,\"pcm_nonzero\":%lu,\"pcm_hash\":%lu,\"pcm_sum\":%lld,"
      "\"heap_internal\":%lu,\"psram\":%lu,\"path\":\"%s\",\"pass\":%s}\n",
      static_cast<unsigned long>(rep), static_cast<unsigned>(index + 1),
      static_cast<unsigned long>(kCases[index].input), static_cast<unsigned long>(kCases[index].output),
      static_cast<unsigned long long>(r.setupUs), static_cast<unsigned long>(r.bytes / 2),
      static_cast<unsigned long>(r.bytes), static_cast<unsigned long long>(r.durationUs),
      static_cast<unsigned long>(r.overflow), r.failed ? "true" : "false",
      r.sessionFailed ? "true" : "false", r.storageFull ? "true" : "false",
      r.cleanup ? "true" : "false", r.timeout ? "true" : "false", r.wav ? "true" : "false",
      r.quiescent ? "true" : "false", static_cast<unsigned long>(r.peak),
      static_cast<unsigned long>(r.nonzero), static_cast<unsigned long>(r.hash), static_cast<long long>(r.sum),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
      path, casePassed(r) ? "true" : "false");
}
} // namespace

bool runPowerSelfTest(CaptureSession &session, Recorder &recorder, AudioCodec &codec,
                      float micGainDb, uint32_t repetitions) {
  const uint64_t used = NoteFiles::usedBytes(), free = NoteFiles::freeBytes();
  Serial.printf("[PowerTestStorage] {\"used\":%llu,\"free\":%llu}\n",
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(free));
  if (!repetitions || !std::isfinite(micGainDb) || session.active() || recorder.isActive() ||
      !session.finished() || !recorder.isFinished()) {
    Serial.printf("[PowerTestError] {\"reason\":\"invalid_or_busy\",\"pass\":false}\n");
    Serial.printf("[PowerTestDone] cases=0 passed=0 failed=0\n");
    return false;
  }

  uint64_t cases = 0, passed = 0;
  bool aborted = false;
  for (uint32_t rep = 0; rep < repetitions && !aborted; ++rep) {
    for (size_t index = 0; index < sizeof(kCases) / sizeof(kCases[0]); ++index) {
      CaseResult result;
      char directory[32], path[64];
      snprintf(directory, sizeof(directory), "/.power-test-%08lx%08lx",
               static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
      snprintf(path, sizeof(path), "%s/r%lu-c%u.wav", directory,
               static_cast<unsigned long>(rep + 1), static_cast<unsigned>(index + 1));
      // Nonce novo por caso inclui rmdir no veredito ANTES de publicar pass.
      // exists ANTES de mkdir: diretorio preexistente nunca vira nossa propriedade.
      const bool owned = !LittleFS.exists(directory) && LittleFS.mkdir(directory);
      if (owned && !NoteFiles::noteExists(path)) {
        const int64_t setupStart = esp_timer_get_time();
        result.started = session.start(codec, kCases[index].input, micGainDb);
        result.setupUs = static_cast<uint64_t>(esp_timer_get_time() - setupStart);
        if (result.started) {
          delay(100);
          result.recorded = recorder.start(path, kCases[index].output, &session);
          if (result.recorded) delay(400);
          else result.failed = true;
          const bool joined = stopAndJoin(session, recorder, result);
          if (joined) {
            if (result.recorded) {
              File reader = NoteFiles::openRead(path);
              result.wav = inspectWav(reader, kCases[index].output, result);
              if (!result.wav) result.failed = true;
              reader.close();
            }
            result.cleanup = NoteFiles::removeWav(path) && !NoteFiles::noteExists(path);
          } else aborted = true;
        } else {
          result.failed = true;
          snapshotFailures(session, recorder, result);
          session.end(); // start falho tambem deve liberar preparacao parcial.
          result.cleanup = NoteFiles::removeWav(path) && !NoteFiles::noteExists(path);
        }
        if (!aborted) result.cleanup = removeOwnDirectory(directory) && result.cleanup;
      } else {
        result.failed = true;
        // Colisao: nem removeWav nem exclusao de conteudo de terceiros.
        result.cleanup = owned && removeOwnDirectory(directory);
      }
      if (!owned || !result.cleanup) aborted = true;
      result.durationUs = static_cast<uint64_t>(result.bytes / 2) * 1000000 / kCases[index].output;
      result.quiescent = codec.quiescent();
      if (!result.quiescent) { result.failed = true; aborted = true; }
      ++cases;
      if (casePassed(result)) ++passed;
      reportCase(rep + 1, index, path, result);
      if (aborted) break;
      delay(1);
    }
  }
  Serial.printf("[PowerTestDone] cases=%llu passed=%llu failed=%llu\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(cases - passed));
  return !aborted && cases == static_cast<uint64_t>(repetitions) * 8 && passed == cases;
}
#endif
