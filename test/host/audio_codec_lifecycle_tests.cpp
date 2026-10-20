#include "../../src/audio/codec.h"
#include "../../src/audio/player.h"
#include "../../src/audio/wav.h"
#include "../../src/audio/speech_leveler.h"
#include "../../src/audio/tone_test.h"
#include "../../src/config/pins.h"
#include <Wire.h>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

extern "C" {
#include "es8311_codec.h"
void *__real_calloc(size_t, size_t);
void __real_free(void *);
}

namespace {
unsigned liveHandles = 0;
bool failAllocation = false;
unsigned allocations = 0;
uint32_t expectedSettle = 0;
bool expectedBoth = false;
unsigned settles = 0;
std::string wav;
bool missingFile = false;
size_t fileReadLimit = SIZE_MAX;
std::string playerPcm;
uint8_t playerDacReg = 0;
void collectPlayerPcm(const void *data, size_t bytes) {
  if (bytes) playerDacReg = Wire.registers[0x32];
  playerPcm.append(static_cast<const char *>(data), bytes);
}

bool muted() { return (Wire.registers[0x31] & 0x60) == 0x60; }
void checkSettle(uint32_t ms) {
  if (!expectedSettle || ms != expectedSettle) return;
  assert(Host::i2s.installed && Host::i2s.config.mode == (I2S_MODE_MASTER | I2S_MODE_TX));
  assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW && muted());
  assert(Wire.registers[0x0D] == (expectedBoth ? 0x01 : 0x31));
  assert(Wire.registers[0x01] == (expectedBoth ? 0x3F : 0x37));
  ++settles;
}
void checkPa(int pin, int level) {
  if (pin != PIN_AUDIO_PA || level == LOW) return;
  assert(Host::i2s.installed);
  assert(Host::i2s.config.mode & I2S_MODE_TX);
  assert(Host::gpioLevels[PIN_AUDIO_PWR] == LOW);
  assert(!muted());
  assert(Wire.registers[0x32] != 0);
}
void reset() {
  assert(liveHandles == 0);
  Wire = TwoWire();
  Host::i2s = Host::I2sState();
  Host::paHighWrites = 0;
  Host::gpioWriteHook = checkPa;
  missingFile = false;
  fileReadLimit = SIZE_MAX;
  failAllocation = false;
  allocations = settles = 0;
  playerDacReg = 0;
  expectedSettle = 0;
  Host::delayHook = checkSettle;
  Host::serialWriteHook = nullptr;
  Host::serialOutput.clear();
  Host::resetClock();
}
void off(AudioCodec &codec) {
  codec.end();
  assert(liveHandles == 0);
  assert(!Host::i2s.installed);
  assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW);
  assert(Host::gpioLevels[PIN_AUDIO_PWR] == HIGH);
  for (int pin : {PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN}) {
    assert(Host::gpioModes[pin] == INPUT);
  }
}
#if ARDUINO_USB_CDC_ON_BOOT
std::string pcm;
bool ready = false;
unsigned readyCount = 0, doneCount = 0;
void checkToneLog(const char *line) {
  if (std::strstr(line, "[ToneReady]")) {
    assert(!ready && Host::i2s.totalWritten == 0);
    assert(Host::i2s.config.mode == (I2S_MODE_MASTER | I2S_MODE_TX));
    ++readyCount;
    ready = true;
  } else if (std::strstr(line, "[ToneDone]")) {
    ++doneCount;
    if (std::strstr(line, "\"quiescent\":true")) {
      assert(liveHandles == 0 && !Host::i2s.installed);
      assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW);
      assert(Host::gpioLevels[PIN_AUDIO_PWR] == HIGH);
    }
  }
}
void collectPcm(const void *data, size_t bytes) {
  assert(ready && liveHandles == 1);
  pcm.append(static_cast<const char *>(data), bytes);
}
void resetTone() {
  reset();
  pcm.clear();
  ready = false;
  readyCount = doneCount = 0;
  Host::serialWriteHook = checkToneLog;
  Host::i2s.writeHook = collectPcm;
}
void checkPcm(const ToneTestParams &p) {
  const size_t frames = 24000 * p.durationMs / 1000;
  assert(pcm.size() == (4096 + frames + 4096) * 4);
  const double amplitude = 32767.0 * std::pow(10.0, p.amplitudeDbfs / 20.0);
  for (size_t frame = 0; frame < pcm.size() / 4; ++frame) {
    int16_t left = 0, right = 0;
    std::memcpy(&left, pcm.data() + frame * 4, sizeof(left));
    std::memcpy(&right, pcm.data() + frame * 4 + 2, sizeof(right));
    assert(left == right);
    if (frame < 4096 || frame >= 4096 + frames) assert(left == 0);
    else {
      const size_t t = frame - 4096;
      double envelope = 1.0;
      if (t < 480) envelope = static_cast<double>(t) / 479;
      if (frames - 1 - t < 480) envelope = static_cast<double>(frames - 1 - t) / 479;
      const double reference = amplitude * envelope * std::sin(6.283185307179586 * p.frequencyHz * t / 24000);
      assert(std::fabs(left - reference) < 3.0); // LUT interpolation + PCM rounding.
    }
  }
}
#endif
}

extern "C" void *__wrap_calloc(size_t count, size_t size) {
  if (failAllocation) return nullptr;
  void *p = __real_calloc(count, size);
  if (p) { ++liveHandles; ++allocations; }
  return p;
}
extern "C" void __wrap_free(void *p) {
  if (p) { assert(liveHandles); --liveHandles; }
  __real_free(p);
}
namespace NoteFiles {
File openRead(const String &) { return missingFile ? File() : File(wav, fileReadLimit); }
}

int main() {
  unsigned passed = 0;
  // Real driver + wrapper: idle begin and repeated begin/end never leak or raise PA.
  {
    reset();
    AudioCodec codec;
    assert(codec.begin());
    assert(codec.begin());
    assert(liveHandles == 1 && Wire.begins == 1);
    assert(Wire.sda == PIN_I2C_SDA && Wire.scl == PIN_I2C_SCL);
    assert(Host::paHighWrites == 0 && muted());
    assert(codec.enable(false));
    assert(!Host::i2s.installed && Wire.registers[0x01] == 0);
    off(codec);
    off(codec);
    ++passed;
  }
  // Capture is ADC + RX only, with DAC clock/reference/reset/PA disabled.
  {
    reset();
    AudioCodec codec;
    assert(codec.setVolume(80)); // Cache must not power the rail.
    assert(liveHandles == 0);
    assert(codec.prepareCapture(16000, 24));
    assert(liveHandles == 1 && Host::i2s.installs == 1);
    assert(Host::i2s.config.mode == (I2S_MODE_MASTER | I2S_MODE_RX));
    assert(Host::i2s.pins.data_out_num == I2S_PIN_NO_CHANGE);
    assert((Wire.registers[0x00] & 0x03) == 0x01);
    assert((Wire.registers[0x01] & 0x0c) == 0x08);
    assert((Wire.registers[0x0d] & 0x38) == 0x08);
    assert(Wire.registers[0x12] == 0x02 && Wire.registers[0x44] == 0x08);
    assert(Wire.registers[0x16] == 4 && Wire.registers[0x18] == 0);
    assert(muted() && Host::paHighWrites == 0);
    const unsigned writes = Wire.writes;
    assert(codec.begin());
    assert(Wire.writes == writes); // Legacy begin must not change an active capture.
    assert(codec.prepareCapture(16000, 24));
    assert(Host::i2s.installs == 1 && liveHandles == 1);
    char buf[8]; size_t count = 999;
    assert(codec.read(buf, sizeof(buf), &count) && count == sizeof(buf));
    assert(!codec.write(buf, sizeof(buf), &count) && count == 0);
    assert(codec.setVolume(100) && muted() && Host::paHighWrites == 0);
    off(codec);
    ++passed;
  }
  // Playback restores volume only after valid TX clocks, then raises PA.
  {
    reset();
    AudioCodec codec;
    assert(codec.preparePlayback(24000, 80));
    assert(Host::i2s.config.mode == (I2S_MODE_MASTER | I2S_MODE_TX));
    assert(Host::i2s.pins.data_in_num == I2S_PIN_NO_CHANGE);
    assert(Host::i2s.config.tx_desc_auto_clear && Host::i2s.config.use_apll);
    assert((Wire.registers[0x01] & 0x0c) == 0x04);
    assert((Wire.registers[0x0e] & 0x70) == 0x70);
    assert(Host::gpioLevels[PIN_AUDIO_PA] == HIGH && !muted());
    uint8_t expected = Wire.registers[0x32];
    assert(codec.enable(false));
    assert(!Host::i2s.installed && Wire.registers[0x32] == 0);
    assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW && muted());
    assert(codec.enable(false));
    assert(Host::i2s.uninstalls == 1);
    assert(codec.enable(true));
    assert(Host::i2s.config.sample_rate == 24000);
    assert(Wire.registers[0x32] == expected && !muted());
    assert(Host::i2s.installs == 2);
    assert(codec.enable(true) && Host::i2s.installs == 2);
    assert(codec.setVolume(0) && muted());
    assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW);
    assert(codec.enable(false) && codec.enable(true) && muted());
    assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW);
    assert(codec.setVolume(80) && Wire.registers[0x32] == expected);
    char buf[8]; size_t count = 123;
    assert(!codec.read(buf, sizeof(buf), &count) && count == 0);
    off(codec);
    ++passed;
  }
  // Mode changes destroy the old handle/driver; same mode/rate does not reinstall.
  {
    reset();
    AudioCodec codec;
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
      assert(codec.prepareCapture(48000, 18));
      assert(codec.preparePlayback(24000, 40));
      assert(liveHandles == 1);
      unsigned installs = Host::i2s.installs;
      assert(codec.preparePlayback(24000, 40));
      assert(Host::i2s.installs == installs);
    }
    off(codec);
    assert(Host::i2s.installs == Host::i2s.uninstalls);
    ++passed;
  }
  // Unsupported clocks must fail (NOT_FOUND is positive esp_err_t, not an index).
  {
    reset();
    AudioCodec codec;
    for (uint32_t rate : {8000U, 16000U, 32000U, 44100U, 48000U}) {
      assert(codec.prepareCapture(rate, 0));
      assert(Host::i2s.config.sample_rate == rate);
      off(codec);
    }
    for (uint32_t rate : {1U, 12345U, 96001U, UINT32_MAX}) {
      assert(!codec.prepareCapture(rate, 0));
      off(codec);
    }
    ++passed;
  }
  // Failures at each hardware boundary clean up and permit a fresh retry.
  {
    for (int failure = 0; failure < 8; ++failure) {
      reset();
      AudioCodec codec;
      if (failure == 0) Wire.beginSuccess = false;
      if (failure == 1) failAllocation = true;
      if (failure == 2) Wire.failWriteReg = 0x44;
      if (failure == 3) Host::i2s.installSuccess = false;
      if (failure == 4) Host::i2s.pinSuccess = false;
      if (failure == 5) Host::i2s.clearSuccess = false;
      if (failure == 6) Wire.failWriteReg = 0x32;
      if (failure == 7) Wire.failReadReg = 0x31;
      assert(!codec.preparePlayback(24000, 80));
      off(codec);
      reset();
      assert(codec.preparePlayback(24000, 80));
      off(codec);
    }
    ++passed;
  }
  // Register failures on resume keep PA disabled and release DMA.
  {
    reset();
    AudioCodec codec;
    assert(codec.preparePlayback(44100, 80));
    assert(codec.enable(false));
    Wire.failWriteReg = 0x32;
    assert(!codec.enable(true));
    assert(!Host::i2s.installed && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
    Wire.failWriteReg = -1;
    assert(codec.enable(true));
    assert(codec.setVolume(80));
    Wire.failWriteReg = 0x31;
    assert(!codec.setVolume(50));
    assert(Host::gpioLevels[PIN_AUDIO_PA] == LOW && codec.volumePercent() == 80);
    Wire.failWriteReg = -1;
    off(codec);
    ++passed;
  }
  // A failed uninstall is retained for retry, never exposing usable read/write.
  {
    reset();
    AudioCodec codec;
    assert(codec.prepareCapture(16000, 0));
    Host::i2s.uninstallSuccess = false;
    assert(!codec.enable(false));
    char buf[4]; size_t count = 99;
    assert(!codec.read(buf, sizeof(buf), &count) && count == 0);
    Host::i2s.uninstallSuccess = true;
    assert(codec.enable(false));
    assert(codec.enable(true));
    off(codec);
    ++passed;
  }
  // Direct C driver also keeps PA low on open/repeated disable and in ADC mode.
  {
    reset();
    uint8_t regs[256] = {};
    static uint8_t *registers;
    registers = regs;
    audio_codec_ctrl_if_t ctrl = {};
    ctrl.is_open = [](const audio_codec_ctrl_if_t *) { return true; };
    ctrl.write_reg = [](const audio_codec_ctrl_if_t *, int reg, int, void *p, int) {
      registers[reg] = *(uint8_t *)p; return 0;
    };
    ctrl.read_reg = [](const audio_codec_ctrl_if_t *, int reg, int, void *p, int) {
      *(uint8_t *)p = registers[reg]; return 0;
    };
    static bool pa;
    audio_codec_gpio_if_t gpio = {};
    gpio.setup = [](int16_t, audio_gpio_dir_t, audio_gpio_mode_t) { return 0; };
    gpio.set = [](int16_t, bool on) { pa = on; return 0; };
    for (auto mode : {ESP_CODEC_DEV_WORK_MODE_ADC, ESP_CODEC_DEV_WORK_MODE_DAC}) {
      pa = true;
      es8311_codec_cfg_t cfg = {};
      cfg.ctrl_if = &ctrl; cfg.gpio_if = &gpio; cfg.pa_pin = PIN_AUDIO_PA;
      cfg.codec_mode = mode; cfg.use_mclk = true;
      auto *driver = es8311_codec_new(&cfg);
      assert(driver && !pa);
      assert(driver->enable(driver, false) == 0 && !pa);
      assert(driver->enable(driver, true) == 0);
      assert(pa == (mode == ESP_CODEC_DEV_WORK_MODE_DAC));
      assert(driver->enable(driver, false) == 0 && !pa);
      assert(driver->enable(driver, false) == 0 && !pa);
      assert(driver->close(driver) == 0);
      std::free(const_cast<audio_codec_if_t *>(driver));
    }
    assert(liveHandles == 0);
    ++passed;
  }
  // Player always ends, handles partial/failed writes, and drains the queued tail.
  {
    WavHeader hdr = makeWavHeader(16000, 1, 8);
    wav.assign((const char *)&hdr, sizeof(hdr));
    wav.append(8, '\x12');
    const std::string original = wav;
    SpeechLeveler reference; assert(reference.configure(16000));
    std::string expected;
    auto append = [&](int16_t value) {
      expected.append(reinterpret_cast<const char *>(&value), sizeof(value));
      expected.append(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    for (int i = 0; i < 4; ++i) append(reference.process(0x1212));
    int16_t tail = 0;
    while (reference.flush(tail)) append(tail);
    assert(reference.hardGuardSamples() == 0);
    expected.append(4096 * 4, '\0');
    for (int failure = -1; failure < 8; ++failure) {
      reset();
      playerPcm.clear(); Host::i2s.writeHook = collectPlayerPcm;
      AudioCodec codec;
      Player player;
      assert(codec.setVolume(40));
      if (failure == 0) missingFile = true;
      if (failure == 1) fileReadLimit = 4;
      if (failure == 2) Host::i2s.installSuccess = false;
      if (failure == 3) Host::i2s.writeSuccess = false;
      if (failure == 4) Host::i2s.writeLimit = 0;
      if (failure == 5) Host::i2s.writeLimit = 3; // Partial DMA writes must not lose bytes.
      if (failure == 6) Host::i2s.failWriteAfter = 16; // Failure on DSP flush, not source.
      if (failure == 7) Host::i2s.failWriteAfter = (4 + reference.latencySamples()) * 4;
      assert(player.play(codec, "/note.wav") == (failure == -1 || failure == 5));
      assert(wav == original); // Header/source are immutable; only playback gets DSP.
      if (failure == -1 || failure == 5) {
        assert(playerPcm == expected && Host::i2s.totalWritten == expected.size());
        assert(playerDacReg == 0x9A); // 40% remains nominal with bounded-speech playback.
#if ARDUINO_USB_CDC_ON_BOOT
        assert(Host::serialOutput.find("[PlaybackSource] kind=wav scope=offered samples=4 ") != std::string::npos);
        assert(Host::serialOutput.find("[PlaybackProcessed] kind=wav scope=offered samples=84 ") != std::string::npos);
        assert(Host::serialOutput.find("latency_samples=80 guards=0") != std::string::npos);
#endif
      }
      off(codec);
    }
    ++passed;
  }
  // Real Player DSP on sustained weak speech and fullscale punches, across reads/retries.
  {
    for (uint32_t rate : {8000U, 11025U, 12000U, 16000U, 22050U, 24000U, 32000U, 44100U, 48000U}) {
      reset(); playerPcm.clear(); Host::i2s.writeHook = collectPlayerPcm;
      Host::i2s.writeLimit = 127;
      WavHeader hdr = makeWavHeader(rate, 1, rate * 2);
      wav.assign(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
      SpeechLeveler reference; assert(reference.configure(rate));
      std::string expected;
      auto append = [&](int16_t value) {
        expected.append(reinterpret_cast<const char *>(&value), sizeof(value));
        expected.append(reinterpret_cast<const char *>(&value), sizeof(value));
      };
      for (uint32_t i = 0; i < rate; ++i) {
        const int16_t sample = i == 0 || i + 1 == rate ? INT16_MIN : i == rate - 2 ? INT16_MAX : 1000;
        wav.append(reinterpret_cast<const char *>(&sample), sizeof(sample));
        append(reference.process(sample));
      }
      int16_t tail = 0;
      while (reference.flush(tail)) append(tail);
      expected.append(4096 * 4, '\0');
      const std::string original = wav;
      AudioCodec codec; Player player; assert(codec.setVolume(100));
      assert(player.play(codec, "/speech.wav"));
      assert(playerPcm == expected && wav == original && reference.hardGuardSamples() == 0);
      assert(playerDacReg == 0xC2); // 100% bounded speech uses the calibrated +4dB curve.
      assert(playerPcm.size() == (rate + reference.latencySamples() + 4096) * 4);
      int16_t middle = 0;
      std::memcpy(&middle, playerPcm.data() + (rate / 2 + reference.latencySamples()) * 4, 2);
      assert(middle >= 1900 && middle <= 1996); // Not a test bypass or unchanged PCM.
      for (size_t i = 0; i < playerPcm.size(); i += 4) {
        int16_t left = 0, right = 0;
        std::memcpy(&left, playerPcm.data() + i, 2); std::memcpy(&right, playerPcm.data() + i + 2, 2);
        assert(left == right && std::abs(int(left)) <= SpeechLeveler::ceilingPcm());
      }
      off(codec);
    }
    // A generic in-range WAV rate is not automatically a supported codec clock.
    reset(); playerPcm.clear(); Host::i2s.writeHook = collectPlayerPcm;
    WavHeader unsupported = makeWavHeader(9999, 1, 2);
    wav.assign(reinterpret_cast<const char *>(&unsupported), sizeof(unsupported));
    wav.append(2, '\x12');
    assert(validWavHeader(unsupported, wav.size()));
    AudioCodec codec; Player player;
    assert(!player.play(codec, "/unsupported.wav"));
    assert(playerPcm.empty() && Host::i2s.installs == 0);
    off(codec);
    ++passed;
  }
  // Calibrated speech curve: only upper volumes change, with real half-dB quantization.
  {
    reset();
    AudioCodec codec;
    uint8_t nominal[101] = {};
    assert(codec.preparePlayback(24000, 100));
    for (unsigned percent = 1; percent <= 100; ++percent) {
      assert(codec.setVolume(static_cast<float>(percent)));
      nominal[percent] = Wire.registers[0x32];
    }
    assert(nominal[100] == 0xBA);
    assert(codec.preparePlayback(24000, 100, true) && Wire.registers[0x32] == 0xC2);
    const float hwGain = 20.0f * std::log10(3.3f / 5.0f) + 6.0f;
    int previous = -1;
    for (unsigned percent = 1; percent <= 100; ++percent) {
      assert(codec.setVolume(static_cast<float>(percent)));
      const float boost = percent <= 60 ? 0.0f : (percent - 60) * 4.0f / 40.0f;
      const int reg = static_cast<int>((40.0f * std::log10(percent / 100.0f) + boost + 95.5f - hwGain) * 2);
      assert(Wire.registers[0x32] == reg && reg >= previous);
      assert(percent <= 60 ? reg == nominal[percent] : reg >= nominal[percent]);
      previous = reg;
    }
    assert(codec.setVolume(80) && Wire.registers[0x32] == 0xB6);
    assert(codec.setVolume(100) && Wire.registers[0x32] == 0xC2);
    assert(codec.enable(false) && codec.enable(true) && Wire.registers[0x32] == 0xC2);
    assert(codec.setSampleRate(48000) && Wire.registers[0x32] == 0xC2);
    const unsigned paWrites = Host::paHighWrites;
    assert(codec.setVolume(0) && muted() && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
    assert(codec.enable(false) && codec.enable(true) && muted());
    assert(codec.setSampleRate(24000) && muted() && Host::paHighWrites == paWrites);
    assert(codec.setVolume(100) && Wire.registers[0x32] == 0xC2);
    assert(codec.preparePlayback(24000, 100) && Wire.registers[0x32] == 0xBA);
    assert(codec.setVolume(80) && Wire.registers[0x32] == nominal[80]);
    assert(codec.preparePlayback(24000, 100, true));
    off(codec);
    assert(codec.begin() && codec.enable(true) && Wire.registers[0x32] == 0xBA);
    off(codec);
    assert(codec.preparePlayback(24000, 100, true));
    const unsigned beforeCapture = Host::paHighWrites;
    assert(codec.prepareCapture(16000, 24) && muted());
    assert(codec.setVolume(100) && codec.enable(false) && codec.enable(true) && muted());
    assert(Host::paHighWrites == beforeCapture && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
    assert(codec.preparePlayback(24000, 100) && Wire.registers[0x32] == 0xBA);
    off(codec);
    ++passed;
  }
  // Diagnostic A/B only changes codec_mode; TX-only and baseline gain are invariant.
  {
    for (bool both : {false, true}) {
      for (uint32_t settle : {0U, 37U, 1000U}) {
        reset();
        AudioCodec codec;
        expectedSettle = settle;
        expectedBoth = both;
        assert(codec.preparePlaybackTest(24000, 100, both, settle));
        assert(Host::i2s.config.mode == (I2S_MODE_MASTER | I2S_MODE_TX));
        assert(Host::i2s.pins.data_in_num == I2S_PIN_NO_CHANGE);
        assert(Wire.registers[0x00] == (both ? 0x80 : 0x82));
        assert(Wire.registers[0x01] == (both ? 0x3F : 0x37));
        assert(Wire.registers[0x0D] == (both ? 0x01 : 0x31));
        assert(Wire.registers[0x0E] == (both ? 0x02 : 0x72));
        // Baseline hw_gain=20log10(3.3/5)+6, driver truncates 0.5dB register steps.
        const float hwGain = 20.0f * std::log10(3.3f / 5.0f) + 6.0f;
        assert(static_cast<int>((95.5f - hwGain) * 2) == 0xBA);
        assert(Wire.registers[0x32] == 0xBA); // NOT0xBF (0dB DAC) or0xFF (+32dB).
        assert(settles == (settle ? 1U : 0U));
        unsigned installs = Host::i2s.installs;
        assert(codec.preparePlaybackTest(24000, 100, both, settle));
        assert(Host::i2s.installs == installs);
        assert(codec.enable(false) && codec.enable(true));
        assert(Host::i2s.config.mode == (I2S_MODE_MASTER | I2S_MODE_TX));
        assert(Wire.registers[0x32] == 0xBA && Wire.registers[0x0D] == (both ? 1 : 0x31));
        assert(settles == (settle ? 2U : 0U));
        char buf[4]; size_t n = 9;
        assert(!codec.read(buf, sizeof(buf), &n) && n == 0);
        off(codec);
        off(codec);
      }
    }
    ++passed;
  }
  // Switching A/B/capture/default recreates the profile and never leaks test fields.
  {
    reset();
    AudioCodec codec;
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
      assert(codec.preparePlaybackTest(24000, 100, true, 37));
      assert(Wire.registers[0x01] == 0x3F);
      assert(codec.preparePlaybackTest(24000, 100, false, 37));
      assert(Wire.registers[0x01] == 0x37);
      assert(codec.prepareCapture(16000, 24));
      assert(codec.preparePlaybackTest(24000, 100, true, 37));
      assert(Wire.registers[0x01] == 0x3F);
      const uint32_t before = millis();
      assert(codec.preparePlayback(24000, 100));
      assert(millis() - before == 10); // Only existing rail delay, no diagnostic settle.
      assert(Wire.registers[0x01] == 0x37 && Wire.registers[0x32] == 0xBA);
      assert(liveHandles == 1);
    }
    off(codec);
    ++passed;
  }
  // Trim is diagnostic only: real half-dB registers, monotone curve, resume/rate and mute.
  {
    const uint8_t expected[] = {0xBA, 0xBE, 0xC2, 0xC6, 0xCA};
    const float hwGain = 20.0f * std::log10(3.3f / 5.0f) + 6.0f;
    for (bool both : {false, true}) {
      for (unsigned i = 0; i < 5; ++i) {
        reset();
        AudioCodec codec;
        const float trim = i * 2.0f;
        assert(codec.preparePlayback(24000, 100, true) && Wire.registers[0x32] == 0xC2);
        assert(codec.preparePlaybackTest(24000, 100, both, 0, trim));
        uint16_t actual = 0;
        assert(codec.readRegister(0x32, actual) && actual == expected[i]);
        assert(codec.enable(false) && codec.enable(true));
        assert(Wire.registers[0x32] == expected[i]);
        assert(codec.setSampleRate(48000) && Wire.registers[0x32] == expected[i]);
        assert(codec.setSampleRate(24000) && Wire.registers[0x32] == expected[i]);
        int previous = -1;
        for (float percent : {20.0f, 40.0f, 60.0f, 80.0f, 100.0f}) {
          assert(codec.setVolume(percent));
          const int reg = static_cast<int>((40.0f * std::log10(percent / 100.0f) + trim + 95.5f - hwGain) * 2);
          assert(Wire.registers[0x32] == reg && reg > previous);
          previous = reg;
        }
        const unsigned paWrites = Host::paHighWrites;
        assert(codec.setVolume(0) && muted() && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
        assert(codec.enable(false) && codec.enable(true));
        assert(codec.setSampleRate(48000) && muted());
        assert(Host::paHighWrites == paWrites && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
        assert(codec.setVolume(100) && Wire.registers[0x32] == expected[i]);
        off(codec);
        assert(codec.preparePlaybackTest(24000, 100, false, 0));
        assert(Wire.registers[0x32] == 0xBA);
        assert(codec.preparePlaybackTest(24000, 100, false, 0, trim));
        assert(codec.preparePlayback(24000, 100, true) && Wire.registers[0x32] == 0xC2);
        assert(codec.setVolume(80) && Wire.registers[0x32] == 0xB6);
        assert(codec.preparePlayback(24000, 100) && Wire.registers[0x32] == 0xBA);
        off(codec);
      }
    }
    ++passed;
  }
  // get_reg is real readback, preserves output on failure; invalid tests fail PA-off.
  {
    reset();
    AudioCodec codec;
    uint16_t value = 999;
    assert(!codec.readRegister(0x32, value) && value == 999);
    assert(codec.preparePlaybackTest(24000, 100, true, 0));
    assert(codec.readRegister(0x32, value) && value == 0xBA);
    Wire.registers[0x32] = 0x55;
    assert(codec.readRegister(0x32, value) && value == 0x55);
    Wire.failReadReg = 0x32;
    assert(!codec.readRegister(0x32, value) && value == 0x55);
    Wire.failReadReg = -1;
    assert(!codec.preparePlaybackTest(24000, 100, false, 1001));
    off(codec);
    for (float invalid : {-1.0f, 101.0f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
      assert(codec.preparePlaybackTest(24000, 100, true, 0));
      assert(!codec.preparePlaybackTest(24000, invalid, true, 0));
      off(codec);
    }
    assert(!codec.preparePlaybackTest(0, 100, false, 0));
    off(codec);
    for (float invalid : {-0.01f, 8.01f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      assert(codec.preparePlaybackTest(24000, 100, false, 0, 8));
      assert(!codec.preparePlaybackTest(24000, 100, false, 0, invalid));
      assert(codec.quiescent() && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
      assert(codec.preparePlayback(24000, 100) && Wire.registers[0x32] == 0xBA);
      off(codec);
    }
    ++passed;
  }
#if ARDUINO_USB_CDC_ON_BOOT
  // Every sequence/profile has identical PCM, complete lead/drain, real logs and cleanup.
  {
    resetTone();
    AudioCodec codec;
    ToneTestParams p;
    p.durationMs = 250;
    p.amplitudeDbfs = -12;
    assert(runToneTest(codec, p));
    const std::string source = pcm;
    resetTone();
    p.speechLeveling = true;
    Host::i2s.writeLimit = 3;
    assert(runToneTest(codec, p));
    SpeechLeveler reference;
    assert(reference.configure(24000));
    std::string expected(4096 * 4, '\0');
    auto append = [&](int16_t value) {
      expected.append(reinterpret_cast<const char *>(&value), sizeof(value));
      expected.append(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    for (size_t i = 0; i < 6000; ++i) {
      int16_t value;
      std::memcpy(&value, source.data() + (4096 + i) * 4, sizeof(value));
      append(reference.process(value));
    }
    int16_t tail;
    while (reference.flush(tail)) append(tail);
    expected.append(4096 * 4, '\0');
    assert(pcm == expected && reference.hardGuardSamples() == 0);
    off(codec);
    resetTone();
    p.both = true;
    assert(!runToneTest(codec, p) && pcm.empty());
    off(codec);
    ++passed;
  }
  {
    for (bool both : {false, true}) {
      for (auto sequence : {ToneTestParams::Sequence::Cold, ToneTestParams::Sequence::CaptureFirst,
                            ToneTestParams::Sequence::Resume}) {
        resetTone();
        AudioCodec codec;
        assert(codec.setVolume(40));
        ToneTestParams p;
        p.both = both; p.durationMs = 250; p.settleMs = 37; p.sequence = sequence;
        expectedSettle = p.settleMs; expectedBoth = p.both;
        Host::i2s.writeLimit = 3; // Crosses frame/sample boundaries, byte-exact continuation.
        assert(runToneTest(codec, p));
        assert(codec.quiescent() && codec.volumePercent() == 40);
        assert(ready && readyCount == 1 && doneCount == 1);
        assert(Host::serialOutput.find("\"dac_trim_db\":0.000,") != std::string::npos);
        assert(Host::serialOutput.find(both ? "\"0x0D\":1," : "\"0x0D\":49,") != std::string::npos);
        assert(Host::serialOutput.find("\"0x32\":186,") != std::string::npos);
        assert(Host::serialOutput.find("\"0x34\":") != std::string::npos);
        assert(Host::serialOutput.find("\"framecount\":6000") != std::string::npos);
        assert(Host::serialOutput.find("\"pass\":true,\"quiescent\":true") != std::string::npos);
        const bool capture = sequence == ToneTestParams::Sequence::CaptureFirst;
        const bool resume = sequence == ToneTestParams::Sequence::Resume;
        assert(Host::i2s.totalRead == (capture ? 6400U : 0U));
        assert(allocations == (capture ? 2U : 1U));
        assert(settles == (resume ? 2U : 1U));
        assert(Host::i2s.installs == (capture || resume ? 2U : 1U));
        checkPcm(p);
        off(codec);
      }
    }
    ++passed;
  }
  // Trim never changes source PCM, including capture/resume and 3-byte partial writes.
  {
    const uint8_t expected[] = {0xBA, 0xBE, 0xC2, 0xC6, 0xCA};
    std::string baseline;
    for (bool both : {false, true}) {
      for (auto sequence : {ToneTestParams::Sequence::Cold, ToneTestParams::Sequence::CaptureFirst,
                            ToneTestParams::Sequence::Resume}) {
        for (unsigned i = 0; i < 5; ++i) {
          resetTone();
          AudioCodec codec;
          assert(codec.setVolume(40));
          ToneTestParams p;
          p.both = both; p.sequence = sequence; p.durationMs = 250;
          p.amplitudeDbfs = -6; p.dacTrimDb = i * 2.0f;
          Host::i2s.writeLimit = 3;
          assert(runToneTest(codec, p));
          assert(readyCount == 1 && doneCount == 1 && codec.quiescent() && codec.volumePercent() == 40);
          assert(Host::serialOutput.find("\"0x32\":" + std::to_string(expected[i]) + ",") != std::string::npos);
          assert(Host::serialOutput.find("\"dac_trim_db\":" + std::to_string(i * 2) + ".000,") != std::string::npos);
          checkPcm(p);
          if (baseline.empty()) baseline = pcm;
          assert(pcm == baseline);
          assert(codec.preparePlayback(24000, 100) && Wire.registers[0x32] == 0xBA);
          off(codec);
        }
      }
    }
    ++passed;
  }
  // Headroom uses actual REG32, not requested trim, and the post-DSP 0.5-FS ceiling.
  {
    for (int scenario = 0; scenario < 7; ++scenario) {
      resetTone();
      AudioCodec codec;
      assert(codec.setVolume(40));
      ToneTestParams p;
      p.durationMs = 250; p.amplitudeDbfs = -3; p.dacTrimDb = 8;
      if (scenario == 1) p.amplitudeDbfs = -6;
      if (scenario == 2) p.speechLeveling = true;
      if (scenario == 3) {
        p.amplitudeDbfs = -6; p.dacTrimDb = 0;
        Host::gpioWriteHook = [](int pin, int level) {
          checkPa(pin, level);
          if (pin == PIN_AUDIO_PA && level == HIGH) Wire.registers[0x32] = 0xD0;
        };
      }
      if (scenario == 4) {
        Host::gpioWriteHook = [](int pin, int level) {
          checkPa(pin, level);
          if (pin == PIN_AUDIO_PA && level == HIGH) Wire.registers[0x32] = 0xBA;
        };
      }
      if (scenario == 5) p.amplitudeDbfs = -5.75f; // Exactly -0.25 after real +5.5dB DAC.
      if (scenario == 6) p.amplitudeDbfs = -5.74f;
      const bool pass = scenario == 1 || scenario == 2 || scenario == 4 || scenario == 5;
      Host::i2s.writeLimit = 3;
      assert(runToneTest(codec, p) == pass);
      assert(doneCount == 1 && readyCount == (pass ? 1U : 0U));
      assert(codec.quiescent() && codec.volumePercent() == 40);
      if (!pass) {
        assert(pcm.empty() && Host::i2s.totalWritten == 0);
        assert(Host::serialOutput.find("\"framecount\":0,\"reason\":\"dac_headroom\"") != std::string::npos);
      } else if (p.speechLeveling) {
        assert(pcm.size() == (4096 + 6000 + 120 + 4096) * 4);
        for (size_t i = 0; i < pcm.size(); i += 4) {
          int16_t left = 0, right = 0;
          std::memcpy(&left, pcm.data() + i, 2); std::memcpy(&right, pcm.data() + i + 2, 2);
          assert(left == right && std::abs(int(left)) <= SpeechLeveler::ceilingPcm());
        }
      } else checkPcm(p);
      off(codec);
    }
    ++passed;
  }
  // Finite/bounded parameters, including invalid sequence, fail without starting PCM.
  {
    auto invalid = [](const ToneTestParams &p) {
      resetTone();
      AudioCodec codec;
      assert(codec.preparePlayback(24000, 80)); // Even pre-existing PA is shut down.
      assert(!runToneTest(codec, p));
      assert(codec.quiescent() && !ready && Host::i2s.totalWritten == 0);
      assert(readyCount == 0 && doneCount == 1);
      assert(Host::serialOutput.find("\"pass\":false") != std::string::npos);
      off(codec);
    };
    ToneTestParams p;
    for (float value : {-48.01f, -2.99f, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      p = ToneTestParams(); p.amplitudeDbfs = value; invalid(p);
    }
    for (float value : {-0.01f, 100.01f, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      p = ToneTestParams(); p.volumePercent = value; invalid(p);
    }
    for (float value : {-0.01f, 8.01f, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      p = ToneTestParams(); p.dacTrimDb = value; invalid(p);
    }
    for (uint32_t value : {0U, 99U, 4001U, UINT32_MAX}) {
      p = ToneTestParams(); p.frequencyHz = value; invalid(p);
    }
    for (uint32_t value : {0U, 249U, 2001U, UINT32_MAX}) {
      p = ToneTestParams(); p.durationMs = value; invalid(p);
    }
    p = ToneTestParams(); p.settleMs = 1001; invalid(p);
    p = ToneTestParams(); p.sequence = static_cast<ToneTestParams::Sequence>(99); invalid(p);
    ++passed;
  }
  // Boundary PCM levels/frequencies and muted volume retain valid protocol/cleanup.
  {
    for (bool high : {false, true}) {
      resetTone();
      AudioCodec codec;
      ToneTestParams p;
      p.amplitudeDbfs = high ? -3 : -48; p.frequencyHz = high ? 4000 : 100;
      p.durationMs = 2000; p.volumePercent = high ? 100 : 0; p.settleMs = 1000;
      assert(runToneTest(codec, p));
      checkPcm(p);
      if (!high) assert(Host::paHighWrites == 0);
      off(codec);
    }
    ++passed;
  }
  // DMA/readback/capture failures stop immediately, always ending PA-off without replay.
  {
    for (int failure = 0; failure < 12; ++failure) {
      resetTone();
      AudioCodec codec;
      ToneTestParams p; p.durationMs = 250;
      if (failure == 0) Host::i2s.writeSuccess = false;
      if (failure == 1) Host::i2s.writeLimit = 0;
      if (failure == 2) Host::i2s.oversizedWrite = true;
      if (failure == 3) Host::i2s.failWriteAfter = 4096 * 4 + 1024;
      if (failure == 4) Host::i2s.failWriteAfter = (4096 + 6000) * 4;
      if (failure == 5) Wire.failReadReg = 0x32;
      if (failure == 6) Host::i2s.installSuccess = false;
      if (failure >= 7 && failure <= 9) {
        p.sequence = ToneTestParams::Sequence::CaptureFirst;
        if (failure == 7) Host::i2s.readSuccess = false;
        if (failure == 8) Host::i2s.readLimit = 0;
        if (failure == 9) Host::i2s.readLimit = 3;
      }
      if (failure == 10) Wire.failWriteReg = 0x32;
      if (failure == 11) Wire.failReadReg = 0x34;
      assert(!runToneTest(codec, p));
      assert(codec.quiescent());
      assert(Host::serialOutput.find("\"pass\":false,\"quiescent\":true") != std::string::npos);
      if (failure >= 5) assert(!ready && Host::i2s.totalWritten == 0);
      off(codec);
    }
    resetTone();
    AudioCodec codec;
    ToneTestParams p; p.durationMs = 250;
    Host::i2s.writeHook = [](const void *data, size_t bytes) {
      collectPcm(data, bytes);
      if (Host::i2s.totalWritten == (4096 + 6000 + 4096) * 4)
        Host::i2s.uninstallSuccess = false;
    };
    assert(!runToneTest(codec, p));
    assert(!codec.quiescent() && Host::gpioLevels[PIN_AUDIO_PA] == LOW);
    assert(Host::serialOutput.find("\"pass\":false,\"quiescent\":false") != std::string::npos);
    Host::i2s.uninstallSuccess = true;
    off(codec);
    ++passed;
  }
#endif
  std::cout << "AudioCodec real driver lifecycle/player/tone tests: " << passed << " groups passed OK\n";
}
