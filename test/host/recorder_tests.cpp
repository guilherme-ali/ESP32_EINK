// Executavel separado; liga src/audio/recorder.cpp e src/storage/note_files.cpp
// reais. Somente o transporte AudioCodec::read, FS e FreeRTOS sao simulados.
// Compilar com -std=c++17 -pthread -ffunction-sections -fdata-sections,
// -Itest/host/stubs, -I<diretorio cJSON.h> e -Wl,--gc-sections.
#include "../../src/audio/recorder.h"
#include "../../src/audio/wav.h"
#include "../../src/storage/note_files.h"
#include <FSImpl.h>
#include <LittleFS.h>
#include <freertos/stream_buffer.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Test { const char *name; std::function<void()> run; };
std::vector<Test> &tests() { static std::vector<Test> all; return all; }
struct Register { Register(const char *name, std::function<void()> run) { tests().push_back({name, run}); } };
#define TEST(name) void name(); Register register_##name(#name, name); void name()
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string("linha ") + std::to_string(__LINE__) + ": " + #c); } while (false)
using Clock = std::chrono::steady_clock;
constexpr const char *wavPath = "/notes/record.wav";
constexpr const char *pcmPath = "/notes/record.pcm";
constexpr const char *tmpPath = "/notes/record.wav.tmp";
constexpr uint32_t rate = 16000;

// Sinal pseudo-aleatorio por indice absoluto. Dois canais diferentes revelam
// troca de canal, perdas, repeticoes, reordenacao e residuos entre gravacoes.
uint16_t sample(size_t frame, uint32_t seed, bool right = false) {
  uint32_t x = static_cast<uint32_t>(frame) ^ seed ^ (right ? 0xd01f713bU : 0x9e3779b9U);
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return static_cast<uint16_t>(x);
}
std::string expectedPcm(size_t frames, uint32_t seed) {
  std::string result(frames * 2, '\0');
  for (size_t i = 0; i < frames; ++i) {
    uint16_t value = sample(i, seed);
    result[2 * i] = static_cast<char>(value); result[2 * i + 1] = static_cast<char>(value >> 8);
  }
  return result;
}
struct Source {
  size_t totalFrames = 0, nextFrame = 0; // escrito somente pela captura.
  uint32_t seed = 1;
  size_t chunkFrames = 512;
  bool paced = true;
  std::atomic<bool> allowRead{false}, eof{false};
  std::atomic<size_t> calls{0}, emittedFrames{0};
  void configure(size_t frames, uint32_t newSeed = 1, bool pacing = true, size_t chunk = 512) {
    totalFrames = frames; nextFrame = 0; seed = newSeed; paced = pacing; chunkFrames = chunk;
    allowRead = false; eof = false; calls = emittedFrames = 0;
  }
} source;

template <typename Predicate>
void waitFor(Predicate predicate, const char *description, std::chrono::milliseconds timeout = std::chrono::seconds(8)) {
  const auto deadline = Clock::now() + timeout;
  while (!predicate()) {
    if (Clock::now() >= deadline) throw std::runtime_error(std::string("timeout: ") + description);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
// Uma task presa constitui falha de seguranca; nao deixar std::thread acessar
// Recorder destruido nem permitir que um timeout transforme isso em UAF.
void waitCleanup(Recorder &recorder) noexcept {
  source.allowRead = true;
  HostRTOS::allowTask("audio_cap"); HostRTOS::allowTask("audio_wr");
  if (recorder.isActive()) {
    recorder.requestStop();
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (!recorder.isFinished()) {
      if (Clock::now() >= deadline) {
        std::cerr << "FALHA: tasks nao terminaram durante cleanup do Recorder\n";
        std::_Exit(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    recorder.finish();
  }
  HostRTOS::joinAll();
}
struct Recording {
  Recorder recorder;
  AudioCodec codec;
  ~Recording() { waitCleanup(recorder); }
  void start(size_t frames, uint32_t seed = 1, const char *path = wavPath, uint32_t sampleRate = rate,
             bool pacing = true, size_t chunk = 512) {
    source.configure(frames, seed, pacing, chunk);
    HostRTOS::allowTask("audio_cap", false); HostRTOS::allowTask("audio_wr", false);
    CHECK(recorder.start(path, sampleRate, &codec));
    CHECK(recorder.isActive());
  }
  void release() {
    source.allowRead = true;
    HostRTOS::allowTask("audio_wr"); HostRTOS::allowTask("audio_cap");
  }
  void captureAll() {
    release(); waitFor([] { return source.eof.load(); }, "codec EOF");
    recorder.requestStop();
    waitFor([this] { return recorder.isFinished(); }, "drain e fim das tasks");
    HostRTOS::joinAll();
  }
  void finalize() {
    CHECK(recorder.isFinished()); recorder.finish();
    CHECK(!recorder.isActive()); CHECK(!HostRTOS::activeTasks.load());
  }
};
WavHeader storedHeader(const char *path = wavPath) {
  const auto raw = LittleFS.bytes(path);
  CHECK(raw.size() == sizeof(WavHeader));
  WavHeader header; std::memcpy(&header, raw.data(), sizeof(header)); return header;
}
std::string headerBytes(const WavHeader &header) {
  return std::string(reinterpret_cast<const char *>(&header), sizeof(header));
}
std::string readAll(File &file, size_t chunk = 733) {
  std::string result;
  std::vector<uint8_t> buffer(chunk);
  size_t expected = file.size();
  while (result.size() < expected) {
    size_t got = file.read(buffer.data(), std::min(chunk, expected - result.size()));
    CHECK(got > 0); result.append(reinterpret_cast<const char *>(buffer.data()), got);
  }
  CHECK(file.available() == 0 && file.read() == -1);
  return result;
}
void checkAudio(const char *path, size_t frames, uint32_t seed, uint32_t sampleRate = rate) {
  const auto pcm = expectedPcm(frames, seed);
  const auto actualPath = NoteFiles::pcmPath(String(path));
  CHECK(LittleFS.bytes(actualPath.c_str()) == pcm); // TODOS os bytes, nao apenas tamanho/hash.
  auto raw = LittleFS.bytes(path); CHECK(raw.size() == sizeof(WavHeader));
  const auto wanted = headerBytes(makeWavHeader(sampleRate, 1, static_cast<uint32_t>(pcm.size())));
  CHECK(raw == wanted);
  CHECK(NoteFiles::validWav(path));
  File virtualWav = NoteFiles::openRead(path);
  CHECK(virtualWav && !virtualWav.isDirectory());
  CHECK(virtualWav.size() == sizeof(WavHeader) + pcm.size());
  CHECK(readAll(virtualWav) == wanted + pcm);
  virtualWav.close();
}
void checkAppendOnly(const char *path = pcmPath) {
  const auto &stats = LittleFS.faults->files.at(path);
  CHECK(stats.writableSeeks == 0 && stats.rewrittenBytes == 0);
  CHECK(!stats.openModes.empty() && stats.openModes.front() == FILE_APPEND);
  for (const auto &mode : stats.openModes) CHECK(mode == FILE_APPEND || mode == FILE_READ);
  CHECK(stats.writtenBytes == LittleFS.bytes(path).size());
}
void checkRecovered(const std::string &original, const std::string &pcm, bool sidecar) {
  LittleFS.put(wavPath, original);
  if (sidecar) LittleFS.put(pcmPath, pcm);
  const auto beforeWav = LittleFS.bytes(wavPath), beforePcm = LittleFS.bytes(pcmPath);
  CHECK(NoteFiles::validWav(wavPath));
  File file = NoteFiles::openRead(wavPath); CHECK(file);
  const auto expected = headerBytes(makeWavHeader(rate, 1, static_cast<uint32_t>(pcm.size()))) + pcm;
  CHECK(file.size() == expected.size() && readAll(file, 17) == expected);
  CHECK(file.seek(40, SeekSet));
  uint8_t cross[19]; CHECK(file.read(cross, sizeof(cross)) == sizeof(cross));
  CHECK(std::memcmp(cross, expected.data() + 40, sizeof(cross)) == 0);
  CHECK(file.seek(3, SeekCur) && file.position() == 62);
  CHECK(file.seek(0, SeekEnd) && file.position() == file.size() && file.read() == -1);
  CHECK(!file.seek(static_cast<uint32_t>(file.size() + 1), SeekSet));
  CHECK(file.seek(0)); char prefix[44]; CHECK(file.readBytes(prefix, sizeof(prefix)) == sizeof(prefix));
  CHECK(std::memcmp(prefix, expected.data(), sizeof(prefix)) == 0);
  file.close();
  CHECK(LittleFS.bytes(wavPath) == beforeWav && LittleFS.bytes(pcmPath) == beforePcm);
  CHECK(LittleFS.faults->renames == 0);
  for (const auto &entry : LittleFS.faults->files) CHECK(entry.second.writes == 0 && entry.second.flushes == 0);
}

struct ProbeImpl : fs::FileImpl {
  size_t cursor = 0, bufferSize = 0, flushes = 0, rewinds = 0;
  long directoryPosition = -1;
  bool opened = true;
  std::string data = "abcdef";
  size_t write(const uint8_t *p, size_t n) override { data.append(reinterpret_cast<const char *>(p), n); return n; }
  size_t read(uint8_t *p, size_t n) override {
    n = std::min(n, data.size() - cursor); std::memcpy(p, data.data() + cursor, n); cursor += n; return n;
  }
  void flush() override { ++flushes; }
  bool seek(uint32_t n, SeekMode mode) override {
    size_t target = n + (mode == SeekCur ? cursor : mode == SeekEnd ? data.size() : 0);
    if (target > data.size()) return false;
    cursor = target; return true;
  }
  size_t position() const override { return cursor; }
  size_t size() const override { return data.size(); }
  bool setBufferSize(size_t n) override { bufferSize = n; return true; }
  void close() override { opened = false; }
  time_t getLastWrite() override { return 123456; }
  const char *path() const override { return "/virtual/test.wav"; }
  const char *name() const override { return "test.wav"; }
  boolean isDirectory() override { return false; }
  fs::FileImplPtr openNextFile(const char *) override { return {}; }
  boolean seekDir(long n) override { directoryPosition = n; return true; }
  String getNextFileName() override { return "child"; }
  String getNextFileName(bool *isDir) override { *isDir = true; return "directory"; }
  void rewindDirectory() override { ++rewinds; }
  operator bool() override { return opened; }
};

TEST(fileimpl_sdk_contract_delegates_every_virtual_operation) {
  auto impl = std::make_shared<ProbeImpl>(); File file(impl);
  CHECK(file && !file.isDirectory() && file.size() == 6 && file.available() == 6);
  CHECK(std::string(file.path()) == "/virtual/test.wav" && std::string(file.name()) == "test.wav");
  CHECK(file.getLastWrite() == 123456 && file.setBufferSize(4096) && impl->bufferSize == 4096);
  CHECK(file.peek() == 'a' && file.position() == 0 && file.read() == 'a');
  CHECK(file.seek(2, SeekCur) && file.position() == 3);
  char bytes[3]; CHECK(file.readBytes(bytes, 3) == 3 && std::string(bytes, 3) == "def");
  CHECK(file.seek(0, SeekEnd) && file.position() == 6);
  CHECK(file.write(uint8_t('g')) == 1); CHECK(file.print("hi") == 2);
  CHECK(file.seek(6) && file.readString() == "ghi");
  file.flush(); CHECK(impl->flushes == 1);
  CHECK(!file.openNextFile() && file.seekDir(11) && impl->directoryPosition == 11);
  bool isDir = false;
  CHECK(file.getNextFileName() == "child" && file.getNextFileName(&isDir) == "directory" && isDir);
  file.rewindDirectory(); CHECK(impl->rewinds == 1);
  File alias = file; file.close(); CHECK(!file && !alias && !impl->opened);
}

TEST(sixty_seconds_16k_integral_left_pcm_checkpoints_yields_no_payload_rewrite) {
  Recording r; r.start(60 * rate, 0x51a7U);
  r.captureAll();
  const auto written = r.recorder.bytesWritten();
  CHECK(written == 1920000 && r.recorder.overflowCount() == 0);
  CHECK(!r.recorder.storageFull() && !r.recorder.writeFailed());
  CHECK(HostRTOS::sentBytes == 3840000 && HostRTOS::receivedBytes == 3840000);
  r.finalize(); CHECK(LittleFS.faults->files.at(pcmPath).seeks == 0);
  checkAudio(wavPath, 60 * rate, 0x51a7U);
  checkAppendOnly();
  const auto &stats = LittleFS.faults->files.at(pcmPath);
  CHECK(stats.flushes >= 50 && stats.flushes <= 65);
  size_t previous = 0;
  for (size_t checkpoint : stats.flushSizes) {
    CHECK(checkpoint >= previous && checkpoint <= written);
    CHECK(checkpoint - previous <= rate * 2 + 8192);
    previous = checkpoint;
  }
  CHECK(previous == written && r.recorder.checkpointBytes() == written);
  CHECK(HostRTOS::delays("audio_wr") + HostRTOS::yields("audio_wr") >= written / 8192);
  CHECK(stats.writes == (written + 8191) / 8192);
  const auto &headerStats = LittleFS.faults->files.at(tmpPath);
  CHECK(headerStats.writes == 2 && headerStats.writtenBytes == 88 && headerStats.rewrittenBytes == 0);
  CHECK(headerStats.writePrefixes.front() == headerBytes(makeWavHeader(rate, 1, 0)));
  CHECK(headerStats.flushes == 2 && !LittleFS.exists(tmpPath));
  CHECK(LittleFS.faults->files.at(wavPath).writes == 0);
  // Exige flush -> close -> reopen/read do PCM antes do segundo commit atomico.
  const auto &events = LittleFS.faults->events;
  size_t close = SIZE_MAX, reopen = SIZE_MAX, finalHeader = SIZE_MAX, flush = SIZE_MAX;
  size_t placeholderCommit = SIZE_MAX, firstPayload = SIZE_MAX;
  size_t headers = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    const auto &e = events[i];
    if (e.path == pcmPath && e.operation == "flush") flush = i;
    if (e.path == pcmPath && e.operation == "close" && close == SIZE_MAX) close = i;
    if (e.path == pcmPath && e.operation == "open:r" && reopen == SIZE_MAX) reopen = i;
    if (e.path == pcmPath && e.operation == "write" && firstPayload == SIZE_MAX) firstPayload = i;
    if (e.path == tmpPath && e.operation == "rename" && placeholderCommit == SIZE_MAX) placeholderCommit = i;
    if (e.path == tmpPath && e.operation == "write" && ++headers == 2) finalHeader = i;
  }
  CHECK(flush < close && close < reopen && reopen < finalHeader);
  CHECK(placeholderCommit < firstPayload);
}

TEST(five_seconds_stop_api_drains_complete_signal) {
  Recording r; r.start(5 * rate, 0x123456U); r.release();
  waitFor([] { return source.eof.load(); }, "cinco segundos de PCM");
  CHECK(r.recorder.stop() == 160000); HostRTOS::joinAll();
  CHECK(!r.recorder.isActive() && !r.recorder.writeFailed() && r.recorder.overflowCount() == 0);
  checkAudio(wavPath, 5 * rate, 0x123456U); checkAppendOnly();
  CHECK(r.recorder.stop() == 160000); // idempotencia e sem nova escrita.
}

TEST(short_consecutive_recordings_do_not_mix_ring_accumulator_or_counters) {
  Recording r;
  const size_t frames[] = {1, 31, 511, 513, 4097, 777};
  const uint32_t rates[] = {8000, 16000, 32000, 44100, 48000, 16000};
  for (size_t i = 0; i < 6; ++i) {
    std::string path = "/notes/short" + std::to_string(i) + ".wav";
    r.start(frames[i], static_cast<uint32_t>(100 + i), path.c_str(), rates[i], true, 37);
    CHECK(r.recorder.bytesWritten() == 0 && r.recorder.overflowCount() == 0);
    r.captureAll(); r.finalize();
    CHECK(r.recorder.bytesWritten() == frames[i] * 2 && !r.recorder.writeFailed());
    checkAudio(path.c_str(), frames[i], static_cast<uint32_t>(100 + i), rates[i]);
    checkAppendOnly(NoteFiles::pcmPath(String(path)).c_str());
  }
}

TEST(request_stop_is_nonblocking_and_tasks_drain_before_finish) {
  Recording r; r.start(0);
  const auto begin = Clock::now(); r.recorder.requestStop();
  CHECK(Clock::now() - begin < std::chrono::milliseconds(200));
  CHECK(!r.recorder.isFinished() && r.recorder.isActive());
  CHECK(HostRTOS::streamDeletes == 0);
  r.release(); waitFor([&r] { return r.recorder.isFinished(); }, "parada assinc sem captura");
  HostRTOS::joinAll(); r.finalize();
  CHECK(source.emittedFrames == 0 && r.recorder.bytesWritten() == 0);
  CHECK(storedHeader().dataSize == 0);
}

TEST(storage_full_autostops_even_boundary_and_keeps_exact_prefix) {
  LittleFS.capacity = NoteFiles::kReserveBytes + sizeof(WavHeader) + 100003;
  Recording r; r.start(std::numeric_limits<size_t>::max(), 27);
  const uint32_t maximum = r.recorder.maximumBytes();
  CHECK(maximum >= rate * 2 && maximum <= 100048 && maximum % 2 == 0);
  r.release(); waitFor([&r] { return r.recorder.isFinished(); }, "storage full auto stop");
  HostRTOS::joinAll(); r.finalize();
  CHECK(r.recorder.storageFull() && !r.recorder.writeFailed() && r.recorder.overflowCount() == 0);
  CHECK(r.recorder.bytesWritten() == maximum && r.recorder.checkpointBytes() == maximum);
  checkAudio(wavPath, maximum / 2, 27); checkAppendOnly();
  CHECK(NoteFiles::freeBytes() >= NoteFiles::kReserveBytes);
}

TEST(full_then_new_recording_resets_storagefull_and_counters) {
  Recording r;
  LittleFS.capacity = NoteFiles::kReserveBytes + sizeof(WavHeader) + 40001;
  r.start(std::numeric_limits<size_t>::max(), 3); r.release();
  waitFor([&r] { return r.recorder.isFinished(); }, "primeira nota full");
  HostRTOS::joinAll(); r.finalize(); CHECK(r.recorder.storageFull());
  LittleFS.capacity = 4 * 1024 * 1024;
  r.start(799, 4, "/notes/next.wav");
  CHECK(!r.recorder.storageFull() && !r.recorder.writeFailed() && r.recorder.bytesWritten() == 0);
  r.captureAll(); r.finalize(); checkAudio("/notes/next.wav", 799, 4);
}

TEST(partial_odd_pcm_write_reports_failure_without_skipping_or_rewriting) {
  LittleFS.faults->pathWriteLimits[pcmPath] = 8191;
  Recording r; r.start(5 * rate, 81); r.release();
  waitFor([&r] { return r.recorder.isFinished(); }, "partial PCM write stop");
  HostRTOS::joinAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && !r.recorder.storageFull());
  CHECK(r.recorder.bytesWritten() == 8190 && LittleFS.bytes(pcmPath) == expectedPcm(4096, 81).substr(0, 8191));
  CHECK(LittleFS.faults->files.at(pcmPath).writes == 1); checkAppendOnly();
  CHECK(storedHeader().dataSize <= r.recorder.bytesWritten() && storedHeader().dataSize % 2 == 0);
}

TEST(zero_pcm_write_autostops_without_busy_retry_or_false_success) {
  LittleFS.faults->pathWriteLimits[pcmPath] = 0;
  Recording r; r.start(5 * rate, 11); r.release();
  waitFor([&r] { return r.recorder.isFinished(); }, "zero write stop");
  HostRTOS::joinAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && r.recorder.bytesWritten() == 0 && !r.recorder.storageFull());
  CHECK(LittleFS.bytes(pcmPath).empty() && storedHeader().dataSize == 0);
  CHECK(LittleFS.faults->files.at(pcmPath).writes == 1); checkAppendOnly();
}

TEST(failed_recording_then_retry_resets_writefailed_and_keeps_new_signal_clean) {
  LittleFS.faults->pathWriteLimits[pcmPath] = 0;
  Recording r; r.start(5 * rate, 12); r.release();
  waitFor([&r] { return r.recorder.isFinished(); }, "primeira nota com falha");
  HostRTOS::joinAll(); r.finalize(); CHECK(r.recorder.writeFailed());
  r.start(1237, 13, "/notes/retry.wav");
  CHECK(!r.recorder.writeFailed() && !r.recorder.storageFull() && r.recorder.bytesWritten() == 0);
  r.captureAll(); r.finalize();
  CHECK(!r.recorder.writeFailed() && r.recorder.overflowCount() == 0);
  checkAudio("/notes/retry.wav", 1237, 13); checkAppendOnly("/notes/retry.pcm");
}

TEST(write_budget_failure_after_checkpoint_retains_contiguous_pcm_prefix) {
  constexpr size_t budget = 4 * 8192 + 776;
  LittleFS.faults->pathWriteBudgets[pcmPath] = budget;
  Recording r; r.start(5 * rate, 15); r.release();
  waitFor([&r] { return r.recorder.isFinished(); }, "write budget exhausted");
  HostRTOS::joinAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && r.recorder.bytesWritten() == budget);
  CHECK(LittleFS.bytes(pcmPath) == expectedPcm(budget / 2, 15)); checkAppendOnly();
  CHECK(LittleFS.faults->files.at(pcmPath).writes == 5 && LittleFS.faults->files.at(pcmPath).flushes >= 1);
}

TEST(final_pcm_flush_size_corruption_fails_reopen_verification) {
  LittleFS.faults->truncateFlush = pcmPath;
  Recording r; r.start(1024, 14); r.captureAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && r.recorder.bytesWritten() == 2048);
  CHECK(LittleFS.bytes(pcmPath).size() == 2047 && storedHeader().dataSize == 0);
  CHECK(LittleFS.faults->files.at(tmpPath).writes == 1);
  CHECK(LittleFS.faults->files.at(pcmPath).rewrittenBytes == 0 && LittleFS.faults->files.at(pcmPath).seeks == 0);
}

TEST(final_header_partial_write_preserves_placeholder_and_recoverable_pcm) {
  LittleFS.faults->pathWriteBudgets[tmpPath] = 44 + 17;
  Recording r; r.start(8193, 77); r.captureAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && storedHeader().dataSize == 0);
  CHECK(LittleFS.bytes(pcmPath) == expectedPcm(8193, 77)); checkAppendOnly();
  File recovered = NoteFiles::openRead(wavPath); CHECK(recovered);
  CHECK(readAll(recovered) == headerBytes(makeWavHeader(rate, 1, 8193 * 2)) + expectedPcm(8193, 77));
}

TEST(final_header_corrupt_flush_preserves_placeholder_and_payload) {
  LittleFS.faults->corruptFlush = tmpPath; LittleFS.faults->corruptFlushAfter = 2;
  Recording r; r.start(5 * rate, 79); r.captureAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && storedHeader().dataSize == 0);
  CHECK(LittleFS.bytes(pcmPath) == expectedPcm(5 * rate, 79)); checkAppendOnly();
  CHECK(NoteFiles::validWav(wavPath));
}

TEST(final_header_rename_failure_never_replaces_old_placeholder) {
  LittleFS.faults->failRename = tmpPath; LittleFS.faults->failRenameAfter = 2;
  Recording r; r.start(517, 80); r.captureAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && storedHeader().dataSize == 0);
  CHECK(LittleFS.bytes(pcmPath) == expectedPcm(517, 80)); checkAppendOnly();
  CHECK(NoteFiles::validWav(wavPath));
}

TEST(capture_saturation_is_terminal_failure_not_silent_discard_loop) {
  Recording r; r.start(std::numeric_limits<size_t>::max(), 72, wavPath, rate, false);
  source.allowRead = true; HostRTOS::allowTask("audio_cap"); // writer permanece bloqueado.
  waitFor([] { return HostRTOS::completed("audio_cap"); }, "capture saturation terminal", std::chrono::seconds(3));
  CHECK(r.recorder.writeFailed() && r.recorder.overflowCount() >= 1);
  CHECK(!r.recorder.isFinished());
  const size_t calls = source.calls.load(), frames = source.emittedFrames.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(source.calls == calls && source.emittedFrames == frames);
  CHECK(calls <= HostRTOS::streamHighWater.load() / 2048 + 2);
  HostRTOS::allowTask("audio_wr");
  waitFor([&r] { return r.recorder.isFinished(); }, "writer drain depois de overflow");
  HostRTOS::joinAll(); r.finalize();
  CHECK(r.recorder.writeFailed() && !r.recorder.storageFull());
  const auto pcm = LittleFS.bytes(pcmPath);
  CHECK(pcm == expectedPcm((pcm.size() + 1) / 2, 72).substr(0, pcm.size()));
}

TEST(capture_and_writer_task_creation_failures_join_before_cleanup_and_retry) {
  for (const char *task : {"audio_cap", "audio_wr"}) {
    LittleFS.reset(); HostRTOS::resetTasks(); HostRTOS::resetStreams();
    LittleFS.put("/notes/keep.wav", "untouched");
    {
      Recording r; source.configure(5 * rate); HostRTOS::failTaskName = task;
      CHECK(!r.recorder.start(wavPath, rate, &r.codec));
      HostRTOS::joinAll();
      CHECK(!r.recorder.isActive() && HostRTOS::activeTasks == 0 && HostRTOS::createFailures == 1);
      CHECK(LittleFS.bytes("/notes/keep.wav") == "untouched");
      CHECK(!LittleFS.exists(wavPath) && !LittleFS.exists(pcmPath) && !LittleFS.exists(tmpPath));
      CHECK(source.emittedFrames == 0);
      HostRTOS::failTaskName.clear();
      r.start(501, 91); r.captureAll(); r.finalize(); checkAudio(wavPath, 501, 91);
    }
    CHECK(HostRTOS::streamCreates == HostRTOS::streamDeletes);
  }
}

TEST(wav_temporary_and_pcm_collisions_refused_without_touching_existing_files) {
  for (const char *collision : {wavPath, tmpPath, pcmPath, "/notes/record.wav.delete",
                               "/notes/record.txt", "/notes/record.md", "/notes/record.sync", "/notes/record.ai"}) {
    LittleFS.reset(); LittleFS.put(collision, "previous recording");
    Recording r; source.configure(1);
    size_t creates = HostRTOS::createCalls;
    CHECK(!r.recorder.start(wavPath, rate, &r.codec));
    CHECK(!r.recorder.isActive() && HostRTOS::createCalls == creates);
    CHECK(LittleFS.bytes(collision) == "previous recording");
    for (const char *other : {wavPath, tmpPath, pcmPath})
      if (std::strcmp(other, collision)) CHECK(!LittleFS.exists(other));
    CHECK(LittleFS.faults->files[collision].writes == 0);
  }
}

TEST(placeholder_open_partial_flush_and_pcm_open_failures_start_no_tasks) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    LittleFS.reset();
    if (fault == 0) LittleFS.faults->failOpen = tmpPath;
    if (fault == 1) LittleFS.faults->pathWriteLimits[tmpPath] = 43;
    if (fault == 2) LittleFS.faults->corruptFlush = tmpPath;
    if (fault == 3) LittleFS.faults->failOpen = pcmPath;
    Recording r; source.configure(512);
    const auto creates = HostRTOS::createCalls;
    CHECK(!r.recorder.start(wavPath, rate, &r.codec));
    CHECK(!r.recorder.isActive() && HostRTOS::createCalls == creates);
    CHECK(!LittleFS.exists(wavPath) && !LittleFS.exists(pcmPath) && !LittleFS.exists(tmpPath));
  }
}

TEST(insufficient_storage_and_stream_allocation_failure_start_no_tasks) {
  for (bool failStream : {false, true}) {
    LittleFS.reset();
    if (!failStream) LittleFS.capacity = NoteFiles::kReserveBytes + 44 + rate;
    HostRTOS::failStreamCreate = failStream;
    Recording r; source.configure(512);
    const auto creates = HostRTOS::createCalls;
    CHECK(!r.recorder.start(wavPath, rate, &r.codec));
    CHECK(!r.recorder.isActive() && HostRTOS::createCalls == creates);
    CHECK(!LittleFS.exists(wavPath) && !LittleFS.exists(pcmPath) && !LittleFS.exists(tmpPath));
    HostRTOS::failStreamCreate = false;
  }
}

TEST(reset_placeholder_plus_pcm_reads_correct_virtual_wav_without_rewrite) {
  const auto pcm = expectedPcm(5003, 65);
  checkRecovered(headerBytes(makeWavHeader(rate, 1, 0)), pcm, true);
}

TEST(legacy_placeholder_with_embedded_pcm_is_recovered_readonly) {
  const auto pcm = expectedPcm(4099, 66);
  checkRecovered(headerBytes(makeWavHeader(rate, 1, 0)) + pcm, pcm, false);
}

TEST(legacy_finalized_wav_still_reads_exactly_without_sidecar_or_rewrite) {
  const auto pcm = expectedPcm(3001, 67);
  checkRecovered(headerBytes(makeWavHeader(rate, 1, static_cast<uint32_t>(pcm.size()))) + pcm, pcm, false);
}
} // namespace

bool AudioCodec::read(void *buffer, size_t length, size_t *bytesRead, uint32_t timeoutMs) {
  (void)timeoutMs;
  ++source.calls; *bytesRead = 0;
  if (!source.allowRead.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return false; }
  if (source.nextFrame >= source.totalFrames) {
    source.eof = true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); return false;
  }
  size_t frames = std::min({length / 4, source.chunkFrames, source.totalFrames - source.nextFrame});
  auto *out = static_cast<uint8_t *>(buffer);
  for (size_t i = 0; i < frames; ++i) {
    uint16_t left = sample(source.nextFrame + i, source.seed);
    uint16_t right = sample(source.nextFrame + i, source.seed, true);
    out[4 * i] = static_cast<uint8_t>(left); out[4 * i + 1] = static_cast<uint8_t>(left >> 8);
    out[4 * i + 2] = static_cast<uint8_t>(right); out[4 * i + 3] = static_cast<uint8_t>(right >> 8);
  }
  source.nextFrame += frames; source.emittedFrames += frames; *bytesRead = frames * 4;
  if (source.paced) std::this_thread::sleep_for(std::chrono::microseconds(100));
  return frames > 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--list") {
    for (const auto &test : tests()) std::cout << test.name << '\n';
    return 0;
  }
  size_t passed = 0, failed = 0, selected = 0;
  for (const auto &test : tests()) {
    if (argc == 2 && std::string(test.name).find(argv[1]) == std::string::npos) continue;
    ++selected;
    HostRTOS::resetTasks(); HostRTOS::resetStreams(); LittleFS.reset();
    Host::resetClock(0x31415926U); // jamais tocado concorrentemente pelos stubs.
    try {
      test.run(); HostRTOS::joinAll();
      CHECK(HostRTOS::activeTasks == 0 && HostRTOS::streamCreates == HostRTOS::streamDeletes);
      CHECK(Host::now == 0x31415926U);
      ++passed; std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception &e) {
      ++failed; std::cout << "FAIL " << test.name << ": " << e.what() << '\n';
    }
  }
  std::cout << "Recorder host (sem hardware): " << passed << '/' << selected << " OK"
            << (failed ? " (FALHOU)" : "") << '\n';
  return failed || !selected ? 1 : 0;
}
