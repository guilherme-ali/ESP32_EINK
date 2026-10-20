#include "../../src/audio/pcm_rate_converter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void check(bool condition) {
  if (!condition) {
    std::cerr << "PCM rate converter check failed\n";
    std::abort();
  }
}

const double kPi = 3.14159265358979323846;
struct Rates { uint32_t in; uint32_t out; };
const Rates kPairs[] = {
  {8000, 8000}, {16000, 16000}, {32000, 32000},
  {44100, 44100}, {48000, 48000}, {16000, 8000},
  {32000, 16000}, {44100, 16000}, {48000, 16000}
};
const Rates kFiltered[] = {
  {16000, 8000}, {32000, 16000}, {44100, 16000}, {48000, 16000}
};

size_t expectedCount(size_t n, Rates rate) {
  return (static_cast<uint64_t>(n) * rate.out + rate.in - 1) / rate.in;
}

int16_t saturate(double value) {
  return static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, std::round(value))));
}

std::vector<int16_t> noise(size_t count) {
  std::vector<int16_t> result(count);
  uint32_t state = 1234567;
  for (auto &sample : result) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<int16_t>(static_cast<int>(state >> 16) - 32768);
  }
  return result;
}

std::vector<int16_t> tone(size_t count, uint32_t hz, double frequency,
                          double amplitude = 12000.0) {
  std::vector<int16_t> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = saturate(amplitude * std::sin(2.0 * kPi * frequency * i / hz + 0.37));
  }
  return result;
}

std::vector<int16_t> drain(PcmRateConverter &converter,
                           const std::vector<int16_t> &input, size_t block) {
  std::vector<int16_t> output;
  output.reserve(input.size());
  int16_t sample = 1234;
  for (size_t start = 0; start < input.size(); start += block) {
    const size_t end = std::min(input.size(), start + block);
    for (size_t i = start; i < end; ++i) {
      const int16_t previous = sample;
      if (converter.push(input[i], sample)) output.push_back(sample);
      else check(sample == previous);
    }
  }
  size_t tail = 0;
  while (converter.finish(sample)) {
    output.push_back(sample);
    ++tail;
    check(tail <= 97); // Even one-sample inputs must terminate with bounded work.
  }
  const int16_t previous = sample;
  check(!converter.finish(sample));
  check(!converter.push(999, sample));
  check(sample == previous);
  return output;
}

std::vector<int16_t> convert(const std::vector<int16_t> &input, Rates rate,
                             size_t block = 257) {
  PcmRateConverter converter;
  check(converter.configure(rate.in, rate.out));
  auto output = drain(converter, input, block);
  check(output.size() == expectedCount(input.size(), rate));
  return output;
}

// Independent double-precision, direct finite-signal FIR. Exact rational output
// times, analytic fractional kernel (not the streaming phase table/ring logic).
std::vector<int16_t> offline(const std::vector<int16_t> &input, Rates rate) {
  std::vector<int16_t> result(expectedCount(input.size(), rate));
  if (rate.in == rate.out) return input;
  const double cutoff = 0.45 * rate.out / rate.in;
  for (size_t k = 0; k < result.size(); ++k) {
    const uint64_t position = static_cast<uint64_t>(k) * rate.in;
    const int64_t center = static_cast<int64_t>(position / rate.out);
    const double fraction = static_cast<double>(position % rate.out) / rate.out;
    double sum = 0.0;
    double gain = 0.0;
    for (int offset = -96; offset <= 96; ++offset) {
      const double x = offset - fraction;
      if (std::abs(x) >= 96.0) continue;
      const double window = 0.42 + 0.5 * std::cos(kPi * x / 96.0) +
                            0.08 * std::cos(2.0 * kPi * x / 96.0);
      const double coefficient = (std::abs(x) < 1e-12 ? 2.0 * cutoff :
                                  std::sin(2.0 * kPi * cutoff * x) / (kPi * x)) * window;
      gain += coefficient;
      const int64_t index = center + offset;
      if (index >= 0 && static_cast<uint64_t>(index) < input.size()) {
        sum += coefficient * input[static_cast<size_t>(index)];
      }
    }
    result[k] = saturate(sum / gain);
  }
  return result;
}

void closeToOffline(const std::vector<int16_t> &input, Rates rate,
                    double rmsLimit = 3.0, int maxLimit = 16) {
  const auto actual = convert(input, rate);
  const auto reference = offline(input, rate);
  check(actual.size() == reference.size());
  double squared = 0.0;
  int maximum = 0;
  for (size_t i = 0; i < actual.size(); ++i) {
    const int difference = std::abs(static_cast<int>(actual[i]) - reference[i]);
    squared += difference * difference;
    maximum = std::max(maximum, difference);
  }
  const double rms = actual.empty() ? 0.0 : std::sqrt(squared / actual.size());
  if (rms > rmsLimit || maximum > maxLimit) {
    std::cerr << "FIR mismatch " << rate.in << "->" << rate.out
              << " RMS=" << rms << " max=" << maximum << '\n';
    check(false);
  }
}

// Coherent-bin projection excludes only the known boundary transient, not
// arbitrary bad blocks. Used for passband, output-frequency and alias checks.
double amplitude(const std::vector<int16_t> &samples, uint32_t hz, double frequency) {
  const size_t trim = 256;
  check(samples.size() > 2 * trim);
  double real = 0.0;
  double imaginary = 0.0;
  for (size_t i = trim; i < samples.size() - trim; ++i) {
    const double angle = 2.0 * kPi * frequency * i / hz;
    real += samples[i] * std::cos(angle);
    imaginary += samples[i] * std::sin(angle);
  }
  return 2.0 * std::hypot(real, imaginary) / (samples.size() - 2 * trim);
}

void testRatesAndLifecycle() {
  static_assert(sizeof(PcmRateConverter) < 14 * 1024, "bounded embedded state");
  PcmRateConverter converter;
  int16_t sample = 987;
  check(!converter.push(123, sample));
  check(!converter.finish(sample));
  check(sample == 987);
  for (Rates rate : kPairs) {
    check(converter.configure(rate.in, rate.out));
    const auto input = noise(2003);
    check(drain(converter, input, 7) == convert(input, rate));
    converter.reset();
    check(drain(converter, input, 511) == convert(input, rate));
    // Reset mid-stream must discard phase, history and pending filter tail.
    converter.reset();
    for (size_t i = 0; i < 371; ++i) converter.push(input[i], sample);
    converter.reset();
    check(drain(converter, input, 31) == convert(input, rate));
  }
  for (Rates rate : {Rates{0, 16000}, Rates{16000, 0}, Rates{16000, 48000},
                     Rates{8000, 16000}, Rates{48000, 8000}, Rates{22050, 16000},
                     Rates{44100, 32000}, Rates{12345, 12345}}) {
    check(!converter.configure(rate.in, rate.out));
    check(!converter.push(123, sample));
    check(!converter.finish(sample));
    converter.reset();
    check(!converter.push(123, sample));
  }
}

void testCountsBypassAndBlocks() {
  for (Rates rate : kPairs) {
    PcmRateConverter converter;
    check(converter.configure(rate.in, rate.out));
    for (size_t n = 0; n <= 402; ++n) {
      converter.reset();
      const auto input = noise(n);
      const auto output = drain(converter, input, 13);
      check(output.size() == expectedCount(n, rate));
      if (rate.in == rate.out) check(output == input);
    }
    const auto input = noise(48017);
    const auto expected = convert(input, rate, 1);
    for (size_t block : {7u, 96u, 97u, 193u, 256u, 1023u, 8192u}) {
      check(convert(input, rate, block) == expected);
    }
  }
  for (Rates rate : kFiltered) {
    PcmRateConverter converter;
    check(converter.configure(rate.in, rate.out));
    int16_t output = 0;
    for (unsigned i = 0; i < 96; ++i) check(!converter.push(1000, output));
    check(converter.push(1000, output));
  }
  for (Rates rate : kPairs) {
    if (rate.in != rate.out) continue;
    std::vector<int16_t> exhaustive;
    for (int i = -32768; i <= 32767; ++i) exhaustive.push_back(static_cast<int16_t>(i));
    check(convert(exhaustive, rate) == exhaustive);
  }
}

void testReferenceEdgesDcAndClipping() {
  for (Rates rate : kFiltered) {
    closeToOffline(noise(3011), rate);
    for (size_t n : {1u, 2u, 3u, 95u, 96u, 97u, 192u, 193u, 194u, 400u}) {
      std::vector<int16_t> impulse(n, 0);
      impulse.front() = 28000;
      closeToOffline(impulse, rate, 1.0, 3);
      check(convert(impulse, rate).front() > 0);
      impulse.front() = 0;
      impulse.back() = 28000;
      closeToOffline(impulse, rate, 1.0, 3);
      const auto tail = convert(impulse, rate);
      check(std::any_of(tail.begin(), tail.end(), [](int16_t x) { return x != 0; }));
    }
    for (int16_t dc : {int16_t{0}, int16_t{10000}, int16_t{-10000},
                       int16_t{32767}, int16_t{-32768}}) {
      const std::vector<int16_t> input(3001, dc);
      const auto output = convert(input, rate);
      for (size_t i = 100; i < output.size() - 100; ++i) {
        check(std::abs(static_cast<int>(output[i]) - dc) <= 1);
      }
      closeToOffline(input, rate, 1.0, 2);
    }
    std::vector<int16_t> steps(4000);
    for (size_t i = 0; i < steps.size(); ++i) steps[i] = i % 400 < 200 ? 32767 : -32768;
    const auto output = convert(steps, rate);
    check(std::count(output.begin(), output.end(), int16_t{32767}) > 10);
    check(std::count(output.begin(), output.end(), int16_t{-32768}) > 10);
    closeToOffline(steps, rate, 3.0, 16);
  }
}

void testSpectralAndContinuity() {
  for (Rates rate : kFiltered) {
    const auto pass = convert(tone(rate.in, rate.in, 1000.0), rate);
    const double gain = amplitude(pass, rate.out, 1000.0) / 12000.0;
    check(gain > 0.995 && gain < 1.005);
    check(amplitude(pass, rate.out, 2000.0) < 2.0);
    const double highPass = rate.out * 0.4;
    const auto high = convert(tone(rate.in, rate.in, highPass), rate);
    check(amplitude(high, rate.out, highPass) / 12000.0 > 0.98);
    // A long input covers many full 160-phase rational cycles and ring wraps.
    closeToOffline(tone(rate.in / 4 + 19, rate.in, 1000.0), rate, 1.0, 2);
    double worstDb = -200.0;
    for (double stop : {rate.out * 0.5, rate.out * 0.505, rate.out * 0.5125,
                        rate.out * 0.55, rate.out * 0.7, rate.in * 0.49}) {
      const auto input = tone(rate.in, rate.in, stop);
      const auto output = convert(input, rate);
      const auto reference = offline(input, rate);
      double alias = std::fmod(stop, rate.out);
      if (alias > rate.out / 2.0) alias = rate.out - alias;
      const double inputAmplitude = amplitude(input, rate.in, stop);
      const double aliasAmplitude = amplitude(output, rate.out, alias);
      const double referenceAmplitude = amplitude(reference, rate.out, alias);
      check(inputAmplitude > 11000.0); // Guard against a vacuous test stimulus.
      const double db = 20.0 * std::log10(std::max(aliasAmplitude, 1e-9) / inputAmplitude);
      worstDb = std::max(worstDb, db);
      check(db < -60.0);
      check(std::abs(aliasAmplitude - referenceAmplitude) < 1.0);
      // Also bound all residual energy, including phase-interpolation images
      // away from the expected folded tone. PCM16 zero is a quantization floor,
      // not a claim of infinite stopband attenuation.
      double energy = 0.0;
      for (size_t i = 256; i < output.size() - 256; ++i) {
        energy += static_cast<double>(output[i]) * output[i];
      }
      check(std::sqrt(energy / (output.size() - 512)) < 12000.0 * 0.001);
    }
    std::cout << rate.in << "->" << rate.out << ": gain1k=" << gain
              << ", worst alias=" << worstDb << " dB\n";
  }
}
} // namespace

int main() {
  testRatesAndLifecycle();
  testCountsBypassAndBlocks();
  testReferenceEdgesDcAndClipping();
  testSpectralAndContinuity();
  std::cout << "PCM rate converter: 4/4 groups OK; state="
            << sizeof(PcmRateConverter) << " bytes\n";
}
