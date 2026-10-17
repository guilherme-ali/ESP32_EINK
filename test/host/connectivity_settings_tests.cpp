#include "../../src/net/settings.h"
#include <Preferences.h>
#include <cassert>
#include <cstring>
#include <iostream>
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
  std::cout << "Connectivity settings: " << passed << "/7 grupos OK\n";
}
