#include "../../src/audio/codec.h"
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

extern "C" {
#include "audio_codec_if.h"
#include "es8311_codec.h"
}

namespace {
struct CodecState {
  bool beginSuccess = true;
  bool muteSuccess = true;
  bool setVolSuccess = true;
  bool enableSuccess = true;
  bool setRegSuccess = true;
  bool muted = false;
  bool enabled = false;
  float lastDb = -999.0f;
  std::vector<std::string> log;

  void reset() {
    beginSuccess = true;
    muteSuccess = true;
    setVolSuccess = true;
    enableSuccess = true;
    setRegSuccess = true;
    muted = false;
    enabled = false;
    lastDb = -999.0f;
    log.clear();
  }
} g_state;

int mockEnable(const audio_codec_if_t *, bool on) {
  g_state.log.push_back(on ? "enable(true)" : "enable(false)");
  if (!g_state.enableSuccess) return ESP_CODEC_DEV_DRV_ERR;
  g_state.enabled = on;
  return ESP_CODEC_DEV_OK;
}

int mockMute(const audio_codec_if_t *, bool mute) {
  g_state.log.push_back(mute ? "mute(true)" : "mute(false)");
  if (!g_state.muteSuccess) return ESP_CODEC_DEV_DRV_ERR;
  g_state.muted = mute;
  return ESP_CODEC_DEV_OK;
}

int mockSetVol(const audio_codec_if_t *, float db) {
  g_state.log.push_back("set_vol(" + std::to_string(db) + ")");
  if (!g_state.setVolSuccess) return ESP_CODEC_DEV_DRV_ERR;
  g_state.lastDb = db;
  return ESP_CODEC_DEV_OK;
}

int mockSetReg(const audio_codec_if_t *, int reg, int value) {
  g_state.log.push_back("set_reg(" + std::to_string(reg) + ", " + std::to_string(value) + ")");
  if (!g_state.setRegSuccess) return ESP_CODEC_DEV_DRV_ERR;
  return ESP_CODEC_DEV_OK;
}

int mockSetFs(const audio_codec_if_t *, esp_codec_dev_sample_info_t *) { return ESP_CODEC_DEV_OK; }
int mockSetMicGain(const audio_codec_if_t *, float) { return ESP_CODEC_DEV_OK; }
int mockClose(const audio_codec_if_t *) { return ESP_CODEC_DEV_OK; }

audio_codec_if_t g_mockInterface = {
  nullptr, // open
  nullptr, // is_open
  mockEnable, // enable
  mockSetFs, // set_fs
  mockMute, // mute
  mockSetVol, // set_vol
  mockSetMicGain, // set_mic_gain
  nullptr, // set_mic_channel_gain
  nullptr, // mute_mic
  mockSetReg, // set_reg
  nullptr, // get_reg
  nullptr, // dump_reg
  mockClose // close
};
} // namespace

extern "C" {
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *) {
  if (!g_state.beginSuccess) return nullptr;
  auto *codec = (audio_codec_if_t *)std::malloc(sizeof(audio_codec_if_t));
  assert(codec);
  *codec = g_mockInterface;
  return codec;
}
}

int main() {
  unsigned passed = 0;

  // 1. Begin initializes and mutes safely
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.begin());
    assert(g_state.muted == true);
    codec.end();
    ++passed;
  }

  // 2. Begin fails closed if driver or mute fails
  {
    g_state.reset();
    g_state.beginSuccess = false;
    AudioCodec codec;
    assert(!codec.begin());

    g_state.reset();
    g_state.muteSuccess = false;
    AudioCodec codec2;
    assert(!codec2.begin());
    codec.end();
    codec2.end();
    ++passed;
  }

  // 3. Volume 0 mutes without unmuting
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.begin());
    g_state.log.clear();

    assert(codec.setVolume(0.0f));
    assert(g_state.muted == true);
    assert(g_state.lastDb == -999.0f); // set_vol must not be called when muted
    assert(g_state.log.size() == 1 && g_state.log[0] == "mute(true)");
    codec.end();
    ++passed;
  }

  // 4. Volume curve is non-positive dB up to 0 dB at 100%
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.begin());

    const struct { float percent; float expectedDb; } tests[] = {
      {20.0f, -27.9588f},
      {40.0f, -15.9176f},
      {60.0f, -8.8739f},
      {80.0f, -3.8764f},
      {100.0f, 0.0f}
    };

    for (const auto &t : tests) {
      g_state.log.clear();
      assert(codec.setVolume(t.percent));
      assert(g_state.muted == false);
      assert(std::fabs(g_state.lastDb - t.expectedDb) < 0.01f);
      assert(g_state.lastDb <= 0.0f); // Crucial: never positive digital gain (no clipping)
      // Must follow sequence: mute(true) -> set_vol -> mute(false)
      assert(g_state.log.size() == 3);
      assert(g_state.log[0] == "mute(true)");
      assert(g_state.log[2] == "mute(false)");
    }
    codec.end();
    ++passed;
  }

  // 5. Invalid values are rejected and do not change state
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.begin());
    assert(codec.setVolume(50.0f));
    const float lastValidDb = g_state.lastDb;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (float invalid : {-0.1f, -10.0f, 100.1f, 150.0f, nan, inf, -inf}) {
      g_state.log.clear();
      assert(!codec.setVolume(invalid));
      assert(g_state.lastDb == lastValidDb);
      assert(g_state.muted == false);
      assert(g_state.log.empty());
    }
    codec.end();
    ++passed;
  }

  // 6. Failures in set_vol or mute leave previous state or fail closed
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.begin());
    assert(codec.setVolume(40.0f));

    // When set_vol fails, setVolume returns false and does not unmute
    g_state.setVolSuccess = false;
    assert(!codec.setVolume(60.0f));
    assert(g_state.muted == true); // DAC remains muted for safety
    g_state.setVolSuccess = true;

    // Can recover by setting a valid volume
    assert(codec.setVolume(40.0f));
    assert(g_state.muted == false);
    codec.end();
    ++passed;
  }

  // 7. enable(false) then enable(true) re-applies volume
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.begin());
    assert(codec.setVolume(80.0f));
    const float expectedDb = g_state.lastDb;

    g_state.log.clear();
    assert(codec.enable(false));
    assert(g_state.enabled == false);

    g_state.lastDb = -999.0f;
    assert(codec.enable(true));
    assert(g_state.enabled == true);
    assert(std::fabs(g_state.lastDb - expectedDb) < 0.01f);
    assert(g_state.muted == false);
    codec.end();
    ++passed;
  }

  // Opt-in calibrated gain applies only above 60%, with a strictly monotone dB curve.
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.preparePlayback(24000, 100, true) && g_state.lastDb == 4);
    float previousDb = -999;
    for (unsigned percent = 1; percent <= 100; ++percent) {
      assert(codec.setVolume(static_cast<float>(percent)));
      const float boost = percent <= 60 ? 0.0f : (percent - 60) * 4.0f / 40.0f;
      const float expected = 40.0f * std::log10(percent / 100.0f) + boost;
      assert(std::fabs(g_state.lastDb - expected) < 0.001f && g_state.lastDb > previousDb);
      previousDb = g_state.lastDb;
    }
    for (float percent : {20.0f, 40.0f, 60.0f}) {
      assert(codec.setVolume(percent));
      assert(std::fabs(g_state.lastDb - 40.0f * std::log10(percent / 100.0f)) < 0.001f);
    }
    assert(codec.setVolume(80) && std::fabs(g_state.lastDb - (-3.8764f + 2)) < 0.001f);
    assert(codec.setVolume(100) && g_state.lastDb == 4);
    assert(codec.enable(false) && codec.enable(true) && g_state.lastDb == 4);
    assert(codec.setSampleRate(48000) && g_state.lastDb == 4);
    assert(codec.setVolume(0) && g_state.muted);
    g_state.lastDb = -999;
    assert(codec.enable(false) && codec.enable(true) && g_state.muted && g_state.lastDb == -999);
    assert(codec.setSampleRate(24000) && g_state.muted && g_state.lastDb == -999);
    assert(codec.setVolume(100) && g_state.lastDb == 4);
    assert(codec.preparePlayback(24000, 100) && g_state.lastDb == 0);
    assert(codec.setVolume(80) && std::fabs(g_state.lastDb + 3.8764f) < 0.001f);
    assert(codec.preparePlayback(24000, 100, true) && g_state.lastDb == 4);
    codec.end();
    assert(codec.begin() && codec.enable(true) && g_state.lastDb == 0); // Legacy stays nominal.
    codec.end();
    assert(codec.preparePlayback(24000, 100, true));
    assert(codec.prepareCapture(16000, 24) && g_state.muted);
    g_state.lastDb = -999;
    assert(codec.setVolume(100) && g_state.muted && g_state.lastDb == -999);
    assert(codec.enable(false) && codec.enable(true) && g_state.muted && g_state.lastDb == -999);
    assert(codec.preparePlayback(24000, 100) && g_state.lastDb == 0);
    codec.end();
    ++passed;
  }
  // Invalid inputs preserve valid opt-in state; hardware failures remain muted.
  {
    g_state.reset();
    AudioCodec codec;
    assert(codec.preparePlayback(24000, 100, true));
    for (float invalid : {-0.01f, 100.01f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      assert(!codec.setVolume(invalid) && g_state.lastDb == 4 && codec.volumePercent() == 100);
      assert(!codec.preparePlayback(24000, invalid, false));
      assert(codec.setVolume(100) && g_state.lastDb == 4);
    }
    assert(!codec.preparePlayback(0, 100, false));
    assert(codec.enable(false) && codec.enable(true) && g_state.lastDb == 4);
    g_state.setVolSuccess = false;
    assert(!codec.setVolume(80) && g_state.muted && codec.volumePercent() == 100);
    g_state.setVolSuccess = true;
    assert(codec.setVolume(100) && g_state.lastDb == 4);
    codec.end();
    assert(!codec.preparePlayback(1, 100, true));
    assert(codec.quiescent());
    assert(codec.preparePlayback(24000, 100) && g_state.lastDb == 0);
    codec.end();
    ++passed;
  }
#if ARDUINO_USB_CDC_ON_BOOT || defined(AUDIO_CODEC_TEST_HOOKS)
  // Diagnostic trim adds only DAC dB; zero percent still never calls set_vol.
  {
    for (float trim : {0.0f, 2.0f, 4.0f, 6.0f, 8.0f}) {
      g_state.reset();
      AudioCodec codec;
      assert(codec.preparePlayback(24000, 100, true) && g_state.lastDb == 4);
      assert(codec.preparePlaybackTest(24000, 100, false, 0, trim));
      float previousDb = -999;
      for (float percent : {20.0f, 40.0f, 60.0f, 80.0f, 100.0f}) {
        assert(codec.setVolume(percent));
        const float expectedDb = 40.0f * std::log10(percent / 100.0f) + trim;
        assert(std::fabs(g_state.lastDb - expectedDb) < 0.001f && g_state.lastDb > previousDb);
        previousDb = g_state.lastDb;
      }
      assert(codec.enable(false) && codec.enable(true) && g_state.lastDb == trim);
      assert(codec.setSampleRate(48000) && g_state.lastDb == trim);
      assert(codec.setVolume(0));
      g_state.lastDb = -999;
      assert(codec.enable(false) && codec.enable(true) && g_state.muted);
      assert(g_state.lastDb == -999);
      assert(codec.setVolume(100) && g_state.lastDb == trim);
      assert(codec.preparePlayback(24000, 100, true) && g_state.lastDb == 4);
      assert(codec.setVolume(80) && std::fabs(g_state.lastDb - (-3.8764f + 2)) < 0.001f);
      assert(codec.preparePlayback(24000, 100) && g_state.lastDb == 0);
      assert(codec.preparePlaybackTest(24000, 100, false, 0, trim));
      codec.end();
      assert(codec.preparePlaybackTest(24000, 100, false, 0) && g_state.lastDb == 0);
      codec.end();
    }
    ++passed;
  }
#endif
  std::cout << "AudioCodec volume tests: " << passed << " groups passed OK\n";
  return 0;
}
