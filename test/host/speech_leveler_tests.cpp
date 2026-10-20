#include "../../src/audio/speech_leveler.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
unsigned checks = 0, groups = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
const double pi = 3.14159265358979323846;
const uint32_t rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};

std::vector<int16_t> render(SpeechLeveler &p, const std::vector<int16_t> &input, size_t block = 256) {
  std::vector<int16_t> output;
  for (size_t at = 0; at < input.size(); at += block)
    for (size_t i = at; i < std::min(at + block, input.size()); ++i) output.push_back(p.process(input[i]));
  int16_t tail = 123;
  size_t flushed = 0;
  while (p.flush(tail)) { output.push_back(tail); ++flushed; }
  CHECK(flushed == p.latencySamples());
  CHECK(!p.flush(tail));
  CHECK(output.size() == input.size() + p.latencySamples());
  CHECK(p.hardGuardSamples() == 0);
  for (int16_t value : output) CHECK(std::abs(int(value)) <= SpeechLeveler::ceilingPcm());
  return output;
}

std::vector<int16_t> tone(uint32_t rate, double db, double seconds = 1, double frequency = 1000) {
  std::vector<int16_t> input(static_cast<size_t>(seconds * rate));
  const double amplitude = 32768 * std::pow(10.0, db / 20);
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<int16_t>(amplitude * std::sin(2 * pi * frequency * i / rate));
  return input;
}

double rms(const std::vector<int16_t> &v, size_t begin, size_t count) {
  double sum = 0;
  for (size_t i = begin; i < begin + count; ++i) sum += double(v[i]) * v[i];
  return std::sqrt(sum / count);
}

double harmonic(const std::vector<int16_t> &v, size_t begin, size_t count, uint32_t rate, unsigned order, double frequency = 1000) {
  double re = 0, im = 0;
  for (size_t i = 0; i < count; ++i) {
    const double phase = 2 * pi * (frequency * order) * i / rate;
    re += v[begin + i] * std::cos(phase);
    im += v[begin + i] * std::sin(phase);
  }
  return std::hypot(re, im) * 2 / count;
}

void configurationAndTail() {
  ++groups;
  CHECK(sizeof(SpeechLeveler) <= 2440);
  SpeechLeveler p;
  int16_t out = 77;
  CHECK(!p.flush(out) && out == 77 && p.process(INT16_MIN) == 0);
  for (uint32_t rate : rates) {
    CHECK(p.configure(rate));
    const size_t latency = (rate + 199) / 200;
    CHECK(p.latencySamples() == latency && latency <= 512);
    for (size_t n : {size_t(0), size_t(1), latency - 1, latency, latency + 1,
                     size_t(255), size_t(256), size_t(257), size_t(1027)}) {
      p.reset();
      std::vector<int16_t> input(n, 47); // Below gate: exact sample/time reference.
      if (n) { input.front() = -53; input.back() = 59; }
      const auto actual = render(p, input);
      if (n) {
        CHECK(std::all_of(actual.begin(), actual.begin() + latency, [](int16_t v) { return v == 0; }));
        CHECK(std::equal(input.begin(), input.end(), actual.begin() + latency));
      } else CHECK(std::all_of(actual.begin(), actual.end(), [](int16_t v) { return v == 0; }));
      CHECK(p.process(1000) == 0); // EOF cannot accidentally begin another stream.
    }
    CHECK(p.configure(rate));
    CHECK(p.process(12) == 0);
  }
  for (uint32_t rate : {0U, 1U, 7999U, 9999U, 12345U, 48001U, 96001U, UINT32_MAX}) {
    CHECK(!p.configure(rate)); CHECK(p.latencySamples() == 0 && p.process(INT16_MAX) == 0);
    CHECK(!p.flush(out) && p.hardGuardSamples() == 0 && p.peakMagnitude() == 0);
  }
  // configure takes integer Hz only: no floating/NaN tuning surface to corrupt state.
}

void gainAndNoise() {
  ++groups;
  for (uint32_t rate : rates) for (bool dc : {false, true}) {
    SpeechLeveler p; CHECK(p.configure(rate));
    auto input = dc ? std::vector<int16_t>(rate, 1000) : tone(rate, -30);
    auto output = render(p, input);
    const size_t n = rate / 4, start = rate * 3 / 4;
    const double gainDb = 20 * std::log10(rms(output, start + p.latencySamples(), n) / rms(input, start, n));
    CHECK(gainDb > 5.94 && gainDb <= 6.0001);
    for (size_t i = 0; i < input.size(); ++i)
      CHECK(std::abs(int(output[i + p.latencySamples()])) <= std::abs(int(input[i])) * std::pow(10.0, 6.0 / 20) + 0.01);
    CHECK(p.configure(rate));
    std::vector<int16_t> noise(rate, 51); // -56 dBFS must not be boosted.
    output = render(p, noise);
    CHECK(std::equal(noise.begin(), noise.end(), output.begin() + p.latencySamples()));
    CHECK(p.configure(rate));
    output = render(p, std::vector<int16_t>(rate, 0));
    CHECK(p.peakMagnitude() == 0);
  }
}

void tonesAndCompression() {
  ++groups;
  for (uint32_t rate : rates) for (double level : {-30.0, -25.0, -22.0, -19.0, -12.0, -6.0, -3.0}) {
    SpeechLeveler p; CHECK(p.configure(rate));
    const auto input = tone(rate, level);
    const auto output = render(p, input);
    const size_t count = rate / 5, at = rate * 4 / 5;
    const double dbIn = 20 * std::log10(rms(input, at, count) / 32768);
    const double over = dbIn + 18;
    const double reduction = over >= 3 ? (2.0 / 3) * over : over > -3 ? (over + 3) * (over + 3) / 18 : 0;
    int peak = 0;
    for (int16_t value : input) peak = std::max(peak, std::abs(int(value)));
    const double peakCapDb = 20 * std::log10((SpeechLeveler::ceilingPcm() - 1.0) / peak);
    const double expected = dbIn + std::min(6 - reduction, peakCapDb);
    const double actual = 20 * std::log10(rms(output, at + p.latencySamples(), count) / 32768);
    CHECK(std::fabs(expected - actual) < 0.12);
    double distortion = 0;
    const unsigned highestHarmonic = std::min(5U, unsigned((rate / 2 - 1) / 1000));
    for (unsigned h = 2; h <= highestHarmonic; ++h) {
      const double component = harmonic(output, at + p.latencySamples(), count, rate, h);
      distortion += component * component;
    }
    const double thd = std::sqrt(distortion) / harmonic(output, at + p.latencySamples(), count, rate, 1);
    CHECK(thd < 0.002); // <0.2%, no per-cycle peak gain oscillation/flattening.
    std::cout << "tone " << rate << " Hz " << level << " dBFS: output=" << actual
              << " dBFS THD=" << thd * 100 << "% guards=" << p.hardGuardSamples() << "\n";
  }
}

void slidingPeakBoundaries() {
  ++groups;
  for (uint32_t rate : rates) {
    struct Guarded {
      uint32_t before = 0x91abcdef;
      SpeechLeveler p;
      uint32_t after = 0x76543210;
    } guarded;
    SpeechLeveler &p = guarded.p;
    CHECK(p.configure(rate));
    const size_t latency = p.latencySamples();
    const size_t attackWindow = (rate + 799) / 800;
    for (size_t separation : {size_t(1), attackWindow - 1, attackWindow, attackWindow + 1,
                              latency - 1, latency, latency + 1, latency * 2 - 1})
      for (int16_t second : {int16_t(8193), int16_t(16384), int16_t(24000), int16_t(INT16_MAX), int16_t(INT16_MIN)}) {
        p.reset();
        std::vector<int16_t> input(rate / 2, 777); // First attain maximum makeup.
        const size_t first = input.size();
        input.resize(first + separation + latency * 3 + 1, 777);
        input[first] = INT16_MIN;
        input[first + separation] = second;
        const auto output = render(p, input);
        CHECK(output[first + latency] < 0);
        CHECK((output[first + separation + latency] < 0) == (second < 0));
        CHECK(guarded.before == 0x91abcdef && guarded.after == 0x76543210);
      }
    // Strictly descending magnitudes fill the entire deque; new maxima then
    // pop many entries. Several wraps exercise raw-slot expiration/queue alias.
    p.reset();
    std::vector<int16_t> input;
    for (size_t i = 0; i < 20 * latency; ++i) {
      const int value = 32767 - static_cast<int>(i % (latency * 2));
      input.push_back(static_cast<int16_t>(i & 1 ? -value : value));
    }
    render(p, input);
    CHECK(guarded.before == 0x91abcdef && guarded.after == 0x76543210);
  }
}

void sustainedToneArtifacts() {
  ++groups;
  for (uint32_t rate : rates) for (double frequency : {100.0, 1000.0, 2000.0})
    for (double level : {-12.0, -6.0, -3.0}) {
      SpeechLeveler p; CHECK(p.configure(rate));
      const auto output = render(p, tone(rate, level, 1, frequency));
      const size_t at = rate * 4 / 5 + p.latencySamples(), count = rate / 5;
      const double fundamental = harmonic(output, at, count, rate, 1, frequency);
      double distortion = 0;
      const unsigned lastHarmonic = std::min(5U, unsigned((rate / 2 - 1) / frequency));
      for (unsigned h = 2; h <= lastHarmonic; ++h) {
        const double amplitude = harmonic(output, at, count, rate, h, frequency);
        distortion += amplitude * amplitude;
      }
      const double thd = std::sqrt(distortion) / fundamental;
      CHECK(thd < 0.002);
      // Energy outside the fundamental also detects AM/sidebands, not just THD.
      const double totalPower = std::pow(rms(output, at, count), 2);
      const double residual = std::sqrt(std::max(0.0, totalPower - fundamental * fundamental / 2));
      CHECK(residual / (fundamental / std::sqrt(2.0)) < 0.003);
    }
}

void crestModeExpiryAndReset() {
  ++groups;
  for (uint32_t rate : rates) {
    SpeechLeveler p, fresh;
    CHECK(p.configure(rate) && fresh.configure(rate));
    const auto suffix = tone(rate, -3, 1, 100);
    const auto reference = render(fresh, suffix);
    std::vector<int16_t> prefix(rate / 5, 1000);
    for (size_t i = 0; i < prefix.size(); i += rate / 100) prefix[i] = INT16_MIN;
    auto input = prefix;
    input.insert(input.end(), suffix.begin(), suffix.end());
    auto output = render(p, input);
    // Dense crests must not leave HIGH latched: at settled 100 Hz the full
    // window produces the same low-distortion waveform as a fresh low-crest tone.
    const size_t count = rate / 5;
    const size_t at = rate * 4 / 5 + p.latencySamples();
    for (size_t i = 0; i < count; ++i)
      CHECK(std::abs(int(output[prefix.size() + at + i]) - int(reference[at + i])) <= 2);

    // Re-enter HIGH after that steady low-crest region, including an EOF peak.
    CHECK(p.configure(rate));
    input.back() = INT16_MIN;
    output = render(p, input);
    CHECK(output.back() < 0 && p.hardGuardSamples() == 0);
    p.reset();
    CHECK(render(p, suffix) == reference); // Reset after EOF clears both queues/flag.

    CHECK(p.configure(rate));
    for (int16_t sample : prefix) p.process(sample);
    p.reset(); // Also clear HIGH during an unfinished stream, not just EOF.
    CHECK(render(p, suffix) == reference);
    p.reset();
    const auto empty = render(p, {});
    CHECK(std::all_of(empty.begin(), empty.end(), [](int16_t sample) { return sample == 0; }));
  }
}

bool voiceLoudnessPolicy() {
  ++groups;
  unsigned failed = 0;
  // Fixed synthetic assumptions, not fitted to the processor's output:
  // ~100 Hz triangular glottal crests, 0.25 ms half-width, harmonic-rich body.
  // Compare syllabically varying crests with dense equal-height crests. Both
  // have source RMS -18 dBFS and peak -3 dBFS; neither is recorded human speech.
  for (uint32_t rate : rates) for (bool dense : {false, true}) {
    const size_t count = rate * 2, period = rate / 100;
    const double crestPeak = 32768 * std::pow(10.0, -3.0 / 20);
    const double targetRms = 32768 * std::pow(10.0, -18.0 / 20);
    std::vector<double> crests(count), body(count);
    double crestPower = 0, bodyPower = 0;
    for (size_t i = 0; i < count; ++i) {
      const size_t phase = i % period;
      const size_t distance = std::min(phase, period - phase);
      const double t = double(i) / rate;
      const double syllable = dense ? 1.0 : 0.2 + 0.8 * std::pow(std::sin(2 * pi * 3 * t), 8);
      const double pulse = std::max(0.0, 1 - distance / (0.00025 * rate));
      crests[i] = crestPeak * syllable * pulse;
      // Disjoint crest/body support fixes the peak while allowing exact source
      // RMS normalization. This normalization never observes processed output.
      if (!pulse) body[i] = std::sin(2 * pi * 100 * t) + 0.35 * std::sin(2 * pi * 300 * t) +
                             0.15 * std::sin(2 * pi * 900 * t);
      crestPower += crests[i] * crests[i];
      bodyPower += body[i] * body[i];
    }
    CHECK(targetRms * targetRms * count > crestPower && bodyPower > 0);
    const double bodyScale = std::sqrt((targetRms * targetRms * count - crestPower) / bodyPower);
    std::vector<int16_t> source(count);
    int peak = 0;
    for (size_t i = 0; i < count; ++i) {
      source[i] = static_cast<int16_t>(crests[i] + bodyScale * body[i]);
      peak = std::max(peak, std::abs(int(source[i])));
    }
    const double rawRms = rms(source, 0, count);
    CHECK(rawRms > 0 && peak > 0);
    const double rawDb = 20 * std::log10(rawRms / 32768);
    const double peakDb = 20 * std::log10(peak / 32768.0);
    CHECK(std::fabs(rawDb + 18) < 0.01 && std::fabs(peakDb + 3) < 0.02);
    SpeechLeveler p; CHECK(p.configure(rate));
    const auto wet = render(p, source);
    const double wetDb = 20 * std::log10(rms(wet, p.latencySamples(), count) / 32768);
    const double gainDb = wetDb - rawDb;
    ++checks;
    const bool pass = gainDb >= 1.0;
    if (!pass) ++failed;
    std::cout << "voice policy " << rate << " Hz " << (dense ? "dense" : "syllabic")
              << ": raw=" << rawDb << " peak=" << peakDb << " wet=" << wetDb
              << " gain=" << gainDb << " dB criterion=+1dB " << (pass ? "PASS" : "FAIL")
              << " guards=" << p.hardGuardSamples() << std::endl;
  }
  std::cout << "Voice loudness policy: " << failed << "/20 cases failed (criterion unchanged)\n";
  return failed == 0;
}

void transientsAndSegmentation() {
  ++groups;
  uint32_t random = 0x128adfe3;
  for (uint32_t rate : rates) {
    SpeechLeveler p; CHECK(p.configure(rate));
    std::vector<int16_t> input(rate, 777);
    for (size_t i = 0; i < rate; ++i) {
      if (i < rate / 3) continue; // First build maximum boost, then attack spikes.
      random = random * 1664525U + 1013904223U;
      input[i] = static_cast<int16_t>(random >> 16);
      if (i % 29 == 0) input[i] = INT16_MIN;
      if (i % 31 == 0) input[i] = INT16_MAX;
    }
    input.back() = INT16_MIN;
    const auto reference = render(p, input, 1);
    CHECK(reference.back() < 0); // Last source sample survives manual flush.
    for (size_t block : {size_t(2), size_t(15), size_t(16), size_t(17), size_t(127), size_t(256), size_t(8192)}) {
      CHECK(p.configure(rate));
      CHECK(render(p, input, block) == reference);
    }
    // Exhaustive PCM transitions in both directions plus a fullscale first sample.
    input.clear(); input.push_back(INT16_MIN);
    for (int value = INT16_MIN; value <= INT16_MAX; ++value) {
      input.push_back(static_cast<int16_t>(value));
      input.push_back(static_cast<int16_t>(-1 - value));
    }
    CHECK(p.configure(rate)); render(p, input);
    for (int16_t step : {int16_t(0), int16_t(1), int16_t(-1), int16_t(8192), int16_t(-8192), int16_t(INT16_MAX), int16_t(INT16_MIN)}) {
      CHECK(p.configure(rate));
      input.assign(rate / 2, 900);
      input.insert(input.end(), rate / 4, step);
      input.insert(input.end(), rate / 4, 0);
      render(p, input);
    }
  }
}
} // namespace

int main() {
  configurationAndTail(); gainAndNoise(); tonesAndCompression(); transientsAndSegmentation();
  slidingPeakBoundaries(); sustainedToneArtifacts(); crestModeExpiryAndReset();
  const bool loudness = voiceLoudnessPolicy();
  std::cout << "SpeechLeveler: " << groups << " groups / " << checks << " checks; safety OK; voice policy "
            << (loudness ? "OK" : "FAIL") << "; sizeof=" << sizeof(SpeechLeveler) << std::endl;
  return loudness ? EXIT_SUCCESS : EXIT_FAILURE;
}
