#include "../../src/net/settings.h"
#include <Preferences.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

int main() {
  unsigned passed = 0;
  Host::resetNvs();
  SettingsStore first;
  assert(first.begin());
  assert(!first.get().assistantEnabled && !first.get().bleFallbackEnabled);
  assert(first.get().blePasskey >= 100000 && first.get().blePasskey <= 999999);
  const uint32_t pin = first.get().blePasskey;
  ++passed;
  assert(first.savePhone(true, 123456));
  Host::rebootNvs();
  SettingsStore restarted;
  assert(restarted.begin());
  assert(restarted.get().bleFallbackEnabled && restarted.get().blePasskey == 123456);
  assert(!restarted.savePhone(true, 1000000));
  assert(restarted.get().blePasskey == 123456 && pin != 0);
  ++passed;
  Host::failPuts.insert("cfg/blePin");
  assert(!restarted.savePhone(false, 654321));
  assert(restarted.get().bleFallbackEnabled && restarted.get().blePasskey == 123456);
  assert(std::get<uint8_t>(Host::nvs["cfg"]["bleFallback"]) == 1);
  Host::failPuts.clear();
  ++passed;
  const char *url = "https://script.google.com/macros/s/deployment_123/exec";
  const std::string token(64, 's');
  assert(restarted.saveAssistant(true, "gemini-3.1-flash-live-preview", url, token.c_str(), "America/Sao_Paulo"));
  Host::rebootNvs();
  SettingsStore stored;
  assert(stored.begin() && stored.get().assistantEnabled);
  assert(!strcmp(stored.get().assistantGatewayToken, token.c_str()));
  assert(!strcmp(stored.get().assistantTimeZone, "America/Sao_Paulo"));
  ++passed;
  for (const char *bad : {"gemini-2.5-pro", "gemini-3.8-live\r\nHost:evil", ""})
    assert(!stored.saveAssistant(true, bad, url, token.c_str(), "America/Sao_Paulo"));
  for (const char *bad : {"http://script.google.com/macros/s/d/exec", "https://evil.example/exec",
                         "https://script.google.com/macros/s/d/exec?token=x"})
    assert(!stored.saveAssistant(true, "gemini-3.1-flash-live-preview", bad, token.c_str(), "America/Sao_Paulo"));
  assert(!stored.saveAssistant(true, "gemini-3.1-flash-live-preview", url, "short", "America/Sao_Paulo"));
  assert(!stored.saveAssistant(true, "gemini-3.1-flash-live-preview", url, token.c_str(), "America/Sao Paulo"));
  ++passed;
  Host::failPuts.insert("cfg/assistZone");
  assert(!stored.saveAssistant(false, "gemini-3.8-live", "", "", "UTC"));
  assert(stored.get().assistantEnabled && !strcmp(stored.get().assistantGatewayUrl, url));
  assert(std::get<std::string>(Host::nvs["cfg"]["assistModel"]) == "gemini-3.1-flash-live-preview");
  assert(std::get<std::string>(Host::nvs["cfg"]["assistToken"]) == token);
  Host::failPuts.clear();
  ++passed;
  assert(stored.saveAssistant(false, "", "", "", "America/Sao_Paulo"));
  assert(!stored.get().assistantEnabled && !stored.get().assistantGatewayToken[0]);
  ++passed;

  Host::resetNvs();
  SettingsStore defaults;
  assert(defaults.get().speakerVolumePercent == 20.0f);
  assert(defaults.begin());
  assert(defaults.get().speakerVolumePercent == 20.0f);
  assert(Host::nvs["cfg"].count("volume") == 0);
  ++passed;

  for (float percent : {20.0f, 40.0f, 100.0f, 0.0f}) {
    assert(defaults.saveVolume(percent));
    assert(defaults.get().speakerVolumePercent == percent);
    assert(std::get<float>(Host::nvs["cfg"]["volume"]) == percent);
    Host::rebootNvs();
    SettingsStore rebooted;
    assert(rebooted.begin());
    assert(rebooted.get().speakerVolumePercent == percent);
  }
  ++passed;

  Host::resetNvs();
  Host::seedNvs("volume", 40.0f);
  SettingsStore legacy;
  assert(legacy.begin());
  assert(legacy.get().speakerVolumePercent == 40.0f);
  assert(std::get<float>(Host::nvs["cfg"]["volume"]) == 40.0f);
  ++passed;

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const auto persisted = Host::nvs;
  for (float invalid : {-1.0f, 100.01f, nan, infinity, -infinity}) {
    assert(!legacy.saveVolume(invalid));
    assert(legacy.get().speakerVolumePercent == 40.0f);
    assert(Host::nvs == persisted);
  }
  Host::rebootNvs();
  SettingsStore afterInvalid;
  assert(afterInvalid.begin());
  assert(afterInvalid.get().speakerVolumePercent == 40.0f);
  ++passed;

  Host::failPuts.insert("cfg/volume");
  assert(!afterInvalid.saveVolume(20.0f));
  assert(afterInvalid.get().speakerVolumePercent == 40.0f);
  assert(Host::nvs == persisted);
  Host::failPuts.clear();
  Host::rebootNvs();
  SettingsStore afterFailure;
  assert(afterFailure.begin());
  assert(afterFailure.get().speakerVolumePercent == 40.0f);
  assert(afterFailure.saveVolume(20.0f));
  assert(afterFailure.get().speakerVolumePercent == 20.0f);
  assert(std::get<float>(Host::nvs["cfg"]["volume"]) == 20.0f);
  ++passed;

  const struct { float raw; float expected; } corrupt[] = {
    {nan, 20.0f}, {infinity, 20.0f}, {-infinity, 20.0f},
    {101.0f, 100.0f}, {-1.0f, 0.0f}
  };
  for (const auto &test : corrupt) {
    Host::resetNvs();
    Host::seedNvs("volume", test.raw);
    SettingsStore sanitized;
    assert(sanitized.begin());
    assert(std::isfinite(sanitized.get().speakerVolumePercent));
    assert(sanitized.get().speakerVolumePercent == test.expected);
    const float raw = std::get<float>(Host::nvs["cfg"]["volume"]);
    assert(std::isnan(test.raw) ? std::isnan(raw) : raw == test.raw);
  }
  ++passed;
  std::cout << "Connectivity settings: " << passed << "/13 grupos OK\n";
}
