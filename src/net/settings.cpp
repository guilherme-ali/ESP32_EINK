#include "settings.h"
#include <Preferences.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_system.h>
#endif

namespace {
Preferences prefs;
constexpr const char *kNamespace = "cfg";
uint32_t driveGeneration = 1;

bool persistString(const char *key, const String &value) {
  prefs.putString(key, value);
  return prefs.isKey(key) && prefs.getString(key, "") == value;
}
bool persistBool(const char *key, bool value) {
  return prefs.putBool(key, value) == 1 && prefs.getBool(key, !value) == value;
}
} // namespace

bool SettingsStore::begin() {
  if (!prefs.begin(kNamespace, false)) return false;
  load();
  return true;
}

void SettingsStore::load() {
  settings_.wifiNetworkCount = prefs.getUChar("wifiCount", 0);
  for (int i = 0; i < Settings::kMaxWifiNetworks; i++) {
    char key[12];
    snprintf(key, sizeof(key), "wifiSsid%d", i);
    prefs.getString(key, settings_.wifiSsid[i], sizeof(settings_.wifiSsid[i]));
    snprintf(key, sizeof(key), "wifiPass%d", i);
    prefs.getString(key, settings_.wifiPass[i], sizeof(settings_.wifiPass[i]));
  }
  if (settings_.wifiNetworkCount == 0) {
    // Migracao unica de versoes anteriores (Fases A-C), que so
    // guardavam uma rede sob as chaves antigas "wifiSsid"/"wifiPass".
    char oldSsid[33] = "";
    char oldPass[65] = "";
    prefs.getString("wifiSsid", oldSsid, sizeof(oldSsid));
    prefs.getString("wifiPass", oldPass, sizeof(oldPass));
    if (oldSsid[0] != '\0') {
      strncpy(settings_.wifiSsid[0], oldSsid, sizeof(settings_.wifiSsid[0]) - 1);
      strncpy(settings_.wifiPass[0], oldPass, sizeof(settings_.wifiPass[0]) - 1);
      settings_.wifiNetworkCount = 1;
      prefs.putString("wifiSsid0", settings_.wifiSsid[0]);
      prefs.putString("wifiPass0", settings_.wifiPass[0]);
      prefs.putUChar("wifiCount", 1);
    }
  }

  prefs.getString("wifiFav", settings_.favoriteWifiSsid, sizeof(settings_.favoriteWifiSsid));

  prefs.getString("sttEndpoint", settings_.sttEndpoint, sizeof(settings_.sttEndpoint));
  String model = prefs.getString("sttModel", settings_.sttModel);
  strncpy(settings_.sttModel, model.c_str(), sizeof(settings_.sttModel) - 1);
  prefs.getString("sttApiKey", settings_.sttApiKey, sizeof(settings_.sttApiKey));
  String trimmedKey(settings_.sttApiKey);
  trimmedKey.trim();
  strlcpy(settings_.sttApiKey, trimmedKey.c_str(), sizeof(settings_.sttApiKey));
  String summary = prefs.getString("summaryModel", settings_.summaryModel);
  strlcpy(settings_.summaryModel, summary.c_str(), sizeof(settings_.summaryModel));
  settings_.sttAutoModel = prefs.getBool("aiAuto", true);
  settings_.geminiFreeOnly = true;
  settings_.geminiFreeConfirmed = prefs.getBool("freeConfirmed", false);
  // Migracao deste aparelho: o proprietario confirmou o projeto gratuito
  // antes da gravacao desta versao. Novas chaves exigem nova confirmacao.
  if (!prefs.isKey("aiPolicyV")) {
    if (!prefs.isKey("freeConfirmed") && strstr(settings_.sttEndpoint, "generativelanguage.googleapis.com") &&
        settings_.sttApiKey[0] && persistBool("freeConfirmed", true)) {
      settings_.geminiFreeConfirmed = true;
    }
    prefs.putUChar("aiPolicyV", 1);
  }
  prefs.getString("driveCliId", settings_.driveClientId, sizeof(settings_.driveClientId));
  prefs.getString("driveCliSec", settings_.driveClientSecret, sizeof(settings_.driveClientSecret));
  prefs.getString("driveRefTok", settings_.driveRefreshToken, sizeof(settings_.driveRefreshToken));
  prefs.getString("driveFolder", settings_.driveFolderId, sizeof(settings_.driveFolderId));
  driveGeneration = settings_.driveAuthGeneration = prefs.getUInt("driveAuthGen", 1);
  settings_.autoSyncEnabled = prefs.getBool("autoSync", false);
  settings_.screensaverTimeoutSec = prefs.getUShort("ssTimeout", 120);
  settings_.audioSampleRateHz = prefs.getUInt("sampleRate", 16000);
  settings_.micGainDb = prefs.getFloat("micGain", 24.0f);
  settings_.speakerVolumeDb = prefs.getFloat("volume", 20.0f);
  settings_.wallpaperChoice = (int8_t)prefs.getChar("wallpaper", -1);
  settings_.showTempHumidity = prefs.getBool("showTempHum", true);
  settings_.bleFallbackEnabled = prefs.getBool("bleFallback", false);
  settings_.blePasskey = prefs.getUInt("blePin", 0);
  if (!prefs.isKey("blePin")) {
#if defined(ARDUINO_ARCH_ESP32)
    settings_.blePasskey = 100000 + esp_random() % 900000;
#else
    settings_.blePasskey = 100000 + static_cast<uint32_t>(random(0, 900000));
#endif
    prefs.putUInt("blePin", settings_.blePasskey);
  }
  settings_.assistantEnabled = prefs.getBool("assistOn", false);
  prefs.getString("assistModel", settings_.assistantModel, sizeof(settings_.assistantModel));
  prefs.getString("assistUrl", settings_.assistantGatewayUrl, sizeof(settings_.assistantGatewayUrl));
  prefs.getString("assistToken", settings_.assistantGatewayToken, sizeof(settings_.assistantGatewayToken));
  String zone = prefs.getString("assistZone", settings_.assistantTimeZone);
  strlcpy(settings_.assistantTimeZone, zone.c_str(), sizeof(settings_.assistantTimeZone));
}

bool SettingsStore::saveWifiNetwork(const char *ssid, const char *pass) {
  int idx = -1;
  for (int i = 0; i < settings_.wifiNetworkCount; i++) {
    if (strcmp(settings_.wifiSsid[i], ssid) == 0) { idx = i; break; }
  }
  if (idx < 0) {
    if (settings_.wifiNetworkCount >= Settings::kMaxWifiNetworks) return false;
    idx = settings_.wifiNetworkCount++;
    prefs.putUChar("wifiCount", settings_.wifiNetworkCount);
  }

  strncpy(settings_.wifiSsid[idx], ssid, sizeof(settings_.wifiSsid[idx]) - 1);
  settings_.wifiSsid[idx][sizeof(settings_.wifiSsid[idx]) - 1] = '\0';
  strncpy(settings_.wifiPass[idx], pass, sizeof(settings_.wifiPass[idx]) - 1);
  settings_.wifiPass[idx][sizeof(settings_.wifiPass[idx]) - 1] = '\0';

  char key[12];
  snprintf(key, sizeof(key), "wifiSsid%d", idx);
  prefs.putString(key, settings_.wifiSsid[idx]);
  snprintf(key, sizeof(key), "wifiPass%d", idx);
  prefs.putString(key, settings_.wifiPass[idx]);
  return true;
}

bool SettingsStore::removeWifiNetwork(int index) {
  if (index < 0 || index >= settings_.wifiNetworkCount) return false;

  if (strcmp(settings_.wifiSsid[index], settings_.favoriteWifiSsid) == 0) {
    saveFavoriteWifi("");
  }

  for (int i = index; i < settings_.wifiNetworkCount - 1; i++) {
    strncpy(settings_.wifiSsid[i], settings_.wifiSsid[i + 1], sizeof(settings_.wifiSsid[i]));
    strncpy(settings_.wifiPass[i], settings_.wifiPass[i + 1], sizeof(settings_.wifiPass[i]));
  }
  settings_.wifiNetworkCount--;
  settings_.wifiSsid[settings_.wifiNetworkCount][0] = '\0';
  settings_.wifiPass[settings_.wifiNetworkCount][0] = '\0';

  for (int i = index; i <= settings_.wifiNetworkCount; i++) {
    char key[12];
    snprintf(key, sizeof(key), "wifiSsid%d", i);
    prefs.putString(key, settings_.wifiSsid[i]);
    snprintf(key, sizeof(key), "wifiPass%d", i);
    prefs.putString(key, settings_.wifiPass[i]);
  }
  prefs.putUChar("wifiCount", settings_.wifiNetworkCount);
  return true;
}

bool SettingsStore::saveFavoriteWifi(const char *ssid) {
  strncpy(settings_.favoriteWifiSsid, ssid, sizeof(settings_.favoriteWifiSsid) - 1);
  settings_.favoriteWifiSsid[sizeof(settings_.favoriteWifiSsid) - 1] = '\0';
  prefs.putString("wifiFav", settings_.favoriteWifiSsid);
  return true;
}

bool SettingsStore::saveStt(const char *endpoint, const char *model, const char *apiKey) {
  if (strlen(endpoint) >= sizeof(settings_.sttEndpoint) ||
      strlen(model) >= sizeof(settings_.sttModel) ||
      strlen(apiKey) >= sizeof(settings_.sttApiKey)) return false;
  String oldEndpoint(settings_.sttEndpoint), oldModel(settings_.sttModel), oldKey(settings_.sttApiKey);
  bool oldConfirmed = settings_.geminiFreeConfirmed;
  bool changed = strcmp(apiKey, settings_.sttApiKey) != 0 || strcmp(endpoint, settings_.sttEndpoint) != 0;
  if (changed) {
    if (!persistBool("freeConfirmed", false)) return false;
  }
  String savedEndpoint(endpoint), savedModel(model), savedKey(apiKey);
  if (!persistString("sttEndpoint", savedEndpoint) || !persistString("sttModel", savedModel) ||
      !persistString("sttApiKey", savedKey)) {
    persistString("sttEndpoint", oldEndpoint); persistString("sttModel", oldModel);
    persistString("sttApiKey", oldKey);
    if (changed) persistBool("freeConfirmed", oldConfirmed);
    return false;
  }
  if (changed) settings_.geminiFreeConfirmed = false;
  strlcpy(settings_.sttEndpoint, savedEndpoint.c_str(), sizeof(settings_.sttEndpoint));
  strlcpy(settings_.sttModel, savedModel.c_str(), sizeof(settings_.sttModel));
  strlcpy(settings_.sttApiKey, savedKey.c_str(), sizeof(settings_.sttApiKey));
  return true;
}

bool SettingsStore::saveAiPolicy(bool automatic, const char *summaryModel, bool freeConfirmed) {
  if (strlen(summaryModel) >= sizeof(settings_.summaryModel)) return false;
  String savedModel(summaryModel);
  bool oldAuto = settings_.sttAutoModel, oldConfirmed = settings_.geminiFreeConfirmed;
  String oldSummary(settings_.summaryModel);
  if (!persistBool("aiAuto", automatic) || !persistBool("freeConfirmed", freeConfirmed) ||
      !persistString("summaryModel", savedModel)) {
    persistBool("aiAuto", oldAuto); persistBool("freeConfirmed", oldConfirmed);
    persistString("summaryModel", oldSummary);
    return false;
  }
  settings_.sttAutoModel = automatic;
  settings_.geminiFreeOnly = true;
  settings_.geminiFreeConfirmed = freeConfirmed;
  strlcpy(settings_.summaryModel, savedModel.c_str(), sizeof(settings_.summaryModel));
  return true;
}

bool SettingsStore::saveDriveApp(const char *clientId, const char *clientSecret) {
  if (strlen(clientId) >= sizeof(settings_.driveClientId) ||
      strlen(clientSecret) >= sizeof(settings_.driveClientSecret)) return false;
  bool changed = strcmp(clientId, settings_.driveClientId) != 0 ||
                 strcmp(clientSecret, settings_.driveClientSecret) != 0;
  String savedId(clientId), savedSecret(clientSecret);
  String oldId(settings_.driveClientId), oldSecret(settings_.driveClientSecret);
  String oldToken(settings_.driveRefreshToken), oldFolder(settings_.driveFolderId);
  uint32_t oldGeneration = driveGeneration;
  if (!persistString("driveCliId", savedId) || !persistString("driveCliSec", savedSecret)) {
    persistString("driveCliId", oldId); persistString("driveCliSec", oldSecret);
    return false;
  }
  if (changed) {
    if (!saveDriveRefreshToken("") || !saveDriveFolderId("")) {
      persistString("driveCliId", oldId); persistString("driveCliSec", oldSecret);
      if (persistString("driveRefTok", oldToken) && persistString("driveFolder", oldFolder) &&
          prefs.putUInt("driveAuthGen", oldGeneration) == sizeof(uint32_t)) {
        strlcpy(settings_.driveRefreshToken, oldToken.c_str(), sizeof(settings_.driveRefreshToken));
        strlcpy(settings_.driveFolderId, oldFolder.c_str(), sizeof(settings_.driveFolderId));
        driveGeneration = settings_.driveAuthGeneration = oldGeneration;
      }
      return false;
    }
  }
  strlcpy(settings_.driveClientId, savedId.c_str(), sizeof(settings_.driveClientId));
  strlcpy(settings_.driveClientSecret, savedSecret.c_str(), sizeof(settings_.driveClientSecret));
  return true;
}

bool SettingsStore::saveDriveRefreshToken(const char *refreshToken) {
  if (strlen(refreshToken) >= sizeof(settings_.driveRefreshToken)) return false;
  String saved(refreshToken);
  String previous(settings_.driveRefreshToken);
  bool previouslyStored = prefs.isKey("driveRefTok");
  if (!persistString("driveRefTok", saved)) return false;
  if (saved != settings_.driveRefreshToken) {
    uint32_t generation = driveGeneration + 1;
    if (!generation) generation = 1;
    if (prefs.putUInt("driveAuthGen", generation) != sizeof(uint32_t) ||
        prefs.getUInt("driveAuthGen", 0) != generation) {
      if (previouslyStored) persistString("driveRefTok", previous);
      else prefs.remove("driveRefTok");
      return false;
    }
    driveGeneration = settings_.driveAuthGeneration = generation;
  }
  strlcpy(settings_.driveRefreshToken, saved.c_str(), sizeof(settings_.driveRefreshToken));
  return true;
}

bool SettingsStore::saveDriveFolderId(const char *folderId) {
  if (strlen(folderId) >= sizeof(settings_.driveFolderId)) return false;
  String saved(folderId);
  if (!persistString("driveFolder", saved)) return false;
  strlcpy(settings_.driveFolderId, saved.c_str(), sizeof(settings_.driveFolderId));
  return true;
}

uint32_t SettingsStore::currentDriveGeneration() { return driveGeneration; }

bool SettingsStore::saveAudio(uint32_t sampleRateHz, float micGainDb) {
  settings_.audioSampleRateHz = sampleRateHz;
  settings_.micGainDb = micGainDb;
  prefs.putUInt("sampleRate", sampleRateHz);
  prefs.putFloat("micGain", micGainDb);
  return true;
}

bool SettingsStore::saveVolume(float volumeDb) {
  settings_.speakerVolumeDb = volumeDb;
  prefs.putFloat("volume", volumeDb);
  return true;
}

bool SettingsStore::saveScreensaverTimeout(uint16_t seconds) {
  settings_.screensaverTimeoutSec = seconds;
  prefs.putUShort("ssTimeout", seconds);
  return true;
}

bool SettingsStore::saveAutoSync(bool enabled) {
  settings_.autoSyncEnabled = enabled;
  prefs.putBool("autoSync", enabled);
  return true;
}

bool SettingsStore::saveWallpaperChoice(int8_t choice) {
  settings_.wallpaperChoice = choice;
  prefs.putChar("wallpaper", choice);
  return true;
}

bool SettingsStore::saveShowTempHumidity(bool enabled) {
  settings_.showTempHumidity = enabled;
  prefs.putBool("showTempHum", enabled);
  return true;
}

bool SettingsStore::savePhone(bool enabled, uint32_t passkey) {
  if (passkey > 999999) return false;
  const bool oldEnabled = settings_.bleFallbackEnabled;
  const uint32_t oldPin = settings_.blePasskey;
  if (!persistBool("bleFallback", enabled) ||
      prefs.putUInt("blePin", passkey) != sizeof(uint32_t) ||
      prefs.getUInt("blePin", UINT32_MAX) != passkey) {
    persistBool("bleFallback", oldEnabled);
    prefs.putUInt("blePin", oldPin);
    return false;
  }
  settings_.bleFallbackEnabled = enabled;
  settings_.blePasskey = passkey;
  return true;
}

bool SettingsStore::saveAssistant(bool enabled, const char *model, const char *url,
                                  const char *token, const char *zone) {
  if (!model || !url || !token || !zone ||
      strlen(model) >= sizeof(settings_.assistantModel) ||
      strlen(url) >= sizeof(settings_.assistantGatewayUrl) ||
      strlen(token) >= sizeof(settings_.assistantGatewayToken) ||
      !zone[0] || strlen(zone) >= sizeof(settings_.assistantTimeZone)) return false;
  const char *normalized = !strncmp(model, "models/", 7) ? model + 7 : model;
  if (model[0] && strcmp(normalized, "gemini-3.8-live") &&
      strcmp(normalized, "gemini-3.1-flash-live-preview")) return false;
  if (enabled && !model[0]) return false;
  for (const char *p = zone; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || strchr("/_+-", *p))) return false;
  if (url[0]) {
    const String endpoint(url);
    if (!endpoint.startsWith("https://script.google.com/macros/s/") ||
        !endpoint.endsWith("/exec") || endpoint.indexOf('?') >= 0 ||
        endpoint.indexOf('#') >= 0 || endpoint.indexOf('\r') >= 0 || endpoint.indexOf('\n') >= 0 ||
        strlen(token) < 32) return false;
  } else if (token[0]) return false;
  const Settings old = settings_;
  if (!persistBool("assistOn", enabled) || !persistString("assistModel", model) ||
      !persistString("assistUrl", url) || !persistString("assistToken", token) ||
      !persistString("assistZone", zone)) {
    persistBool("assistOn", old.assistantEnabled);
    persistString("assistModel", old.assistantModel);
    persistString("assistUrl", old.assistantGatewayUrl);
    persistString("assistToken", old.assistantGatewayToken);
    persistString("assistZone", old.assistantTimeZone);
    return false;
  }
  settings_.assistantEnabled = enabled;
  strlcpy(settings_.assistantModel, model, sizeof(settings_.assistantModel));
  strlcpy(settings_.assistantGatewayUrl, url, sizeof(settings_.assistantGatewayUrl));
  strlcpy(settings_.assistantGatewayToken, token, sizeof(settings_.assistantGatewayToken));
  strlcpy(settings_.assistantTimeZone, zone, sizeof(settings_.assistantTimeZone));
  return true;
}
