// Exercita as funcoes REAIS de parsing/veredito. Sem hardware, Recorder fake
// ou implementacao paralela do parser; o linker remove o comando nao chamado.
#include "../../src/audio/power_selftest.cpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) throw std::runtime_error( \
  std::string("linha ") + std::to_string(__LINE__) + ": " + #condition); } while (false)

std::string wavBytes(const WavHeader &header, const std::vector<int16_t> &pcm) {
  std::string bytes(reinterpret_cast<const char *>(&header), sizeof(header));
  for (int16_t sample : pcm) {
    const uint16_t raw = static_cast<uint16_t>(sample);
    bytes.push_back(static_cast<char>(raw & 255));
    bytes.push_back(static_cast<char>(raw >> 8));
  }
  return bytes;
}

void rateMatrixAndStats() {
  CHECK(sizeof(kCases) / sizeof(kCases[0]) == 8);
  const uint32_t expectedIn[] = {16000, 16000, 32000, 44100, 48000, 32000, 44100, 48000};
  const uint32_t expectedOut[] = {8000, 16000, 32000, 44100, 48000, 16000, 16000, 16000};
  for (size_t i = 0; i < 8; ++i) {
    CHECK(kCases[i].input == expectedIn[i] && kCases[i].output == expectedOut[i]);
    const uint32_t rate = kCases[i].output;
    const std::vector<int16_t> pcm = {-32768, -1, 0, 1, 32767};
    const std::string bytes = wavBytes(makeWavHeader(rate, 1, 10), pcm);
    for (size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(7), size_t(512)}) {
      File reader(bytes, chunk);
      CaseResult r; r.bytes = 10;
      CHECK(inspectWav(reader, rate, r));
      CHECK(r.samples == 5 && r.nonzero == 4 && r.peak == 32768 && r.sum == -1);
      CHECK(r.durationUs == 5000000U / rate);
      CHECK(r.hash == 0x8af49a58U); // FNV-1a dos dez bytes PCM little-endian.
      CHECK(reader.position() == bytes.size());
    }
  }
}

void silenceAndBoundaries() {
  for (uint32_t rate : {8000U, 16000U, 32000U, 44100U, 48000U}) {
    std::vector<int16_t> pcm(rate / 2, 0);
    const uint32_t size = static_cast<uint32_t>(pcm.size() * 2);
    File reader(wavBytes(makeWavHeader(rate, 1, size), pcm), 511);
    CaseResult r; r.bytes = size;
    CHECK(inspectWav(reader, rate, r));
    CHECK(r.samples == rate / 2 && r.durationUs == 500000);
    CHECK(!r.peak && !r.nonzero && !r.sum);
    r.started = r.recorded = r.wav = r.cleanup = r.quiescent = true;
    CHECK(casePassed(r)); // Silencio nao significa falha nem valida som analogico.
  }
  std::vector<int16_t> pcm(257, 0);
  pcm.front() = -32768; pcm.back() = 32767;
  for (size_t chunk : {size_t(1), size_t(255), size_t(256), size_t(511), size_t(512), size_t(513)}) {
    File reader(wavBytes(makeWavHeader(44100, 1, 514), pcm), chunk);
    CaseResult r; r.bytes = 514;
    CHECK(inspectWav(reader, 44100, r));
    CHECK(r.samples == 257 && r.nonzero == 2 && r.peak == 32768 && r.sum == -1);
    CHECK(r.durationUs == 5827);
  }
}

void invalidHeadersAndLengths() {
  const std::vector<int16_t> pcm = {1, -2, 3, -4};
  WavHeader header = makeWavHeader(16000, 1, 8);
  auto reject = [&](const WavHeader &h, const std::string &bytes, uint32_t rate = 16000, uint32_t written = 8) {
    (void)h;
    File reader(bytes); CaseResult r; r.bytes = written;
    CHECK(!inspectWav(reader, rate, r));
  };
  const std::vector<std::function<void(WavHeader &)>> mutations = {
      [](WavHeader &h) { h.riff[0] = 'X'; }, [](WavHeader &h) { h.wave[0] = 'X'; },
      [](WavHeader &h) { h.fmt[0] = 'X'; }, [](WavHeader &h) { h.data[0] = 'X'; },
      [](WavHeader &h) { h.fmtSize = 18; }, [](WavHeader &h) { h.audioFormat = 3; },
      [](WavHeader &h) { h.numChannels = 2; }, [](WavHeader &h) { h.bitsPerSample = 8; },
      [](WavHeader &h) { h.blockAlign = 4; }, [](WavHeader &h) { h.byteRate = 1; },
      [](WavHeader &h) { h.sampleRate = 0; }, [](WavHeader &h) { h.sampleRate = 48001; },
      [](WavHeader &h) { ++h.chunkSize; }, [](WavHeader &h) { --h.dataSize; },
      [](WavHeader &h) { h.dataSize = 0; h.chunkSize = 36; }};
  for (const auto &mutate : mutations) {
    WavHeader broken = header; mutate(broken); reject(broken, wavBytes(broken, pcm));
  }
  const std::string valid = wavBytes(header, pcm);
  for (size_t length = 0; length < valid.size(); ++length) reject(header, valid.substr(0, length));
  reject(header, valid + "x");
  reject(header, valid, 8000);
  reject(header, valid, 16000, 6);
  File absent; CaseResult r; r.bytes = 8;
  CHECK(!inspectWav(absent, 16000, r));
}

void readFailureAndDeadline() {
  const std::string bytes = wavBytes(makeWavHeader(16000, 1, 4), {123, -123});
  fs::FS storage;
  storage.put("/test.wav", bytes);
  storage.faults->readBudget = 45; // Cabecalho completo + apenas um byte PCM.
  File reader = storage.open("/test.wav"); CaseResult truncated; truncated.bytes = 4;
  CHECK(!inspectWav(reader, 16000, truncated));
  reader.close();
  File timed(bytes); CaseResult timeout; timeout.bytes = 4;
  HostPowerTest::timerStepUs = kDeadlineUs;
  CHECK(!inspectWav(timed, 16000, timeout));
  HostPowerTest::timerStepUs = 0;
  File zeroRead(bytes, 0); CaseResult zero; zero.bytes = 4;
  CHECK(!inspectWav(zeroRead, 16000, zero));
}

void everyFailureVetoesPass() {
  CaseResult good;
  good.started = good.recorded = good.wav = good.cleanup = good.quiescent = true;
  CHECK(casePassed(good));
  for (bool CaseResult::*flag : {&CaseResult::started, &CaseResult::recorded, &CaseResult::wav,
                                &CaseResult::cleanup, &CaseResult::quiescent}) {
    CaseResult broken = good; broken.*flag = false; CHECK(!casePassed(broken));
  }
  for (bool CaseResult::*flag : {&CaseResult::failed, &CaseResult::sessionFailed,
                                &CaseResult::storageFull, &CaseResult::timeout}) {
    CaseResult broken = good; broken.*flag = true; CHECK(!casePassed(broken));
  }
  CaseResult overflow = good; overflow.overflow = 1; CHECK(!casePassed(overflow));
  good.peak = good.nonzero = good.hash = 0;
  CHECK(casePassed(good));
}
} // namespace

int main() {
  try {
    rateMatrixAndStats(); silenceAndBoundaries(); invalidHeadersAndLengths();
    readFailureAndDeadline(); everyFailureVetoesPass();
    std::cout << "Power self-test parser/resultados: 5 grupos, " << checks << " checks OK\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
