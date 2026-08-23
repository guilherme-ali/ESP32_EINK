#include "settings.h"
#include <Preferences.h>

namespace {
Preferences prefs;
constexpr const char *kNamespace = "cfg";
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
  prefs.getString("driveCliId", settings_.driveClientId, sizeof(settings_.driveClientId));
  prefs.getString("driveCliSec", settings_.driveClientSecret, sizeof(settings_.driveClientSecret));
  prefs.getString("driveRefTok", settings_.driveRefreshToken, sizeof(settings_.driveRefreshToken));
  prefs.getString("driveFolder", settings_.driveFolderId, sizeof(settings_.driveFolderId));
  settings_.autoSyncEnabled = prefs.getBool("autoSync", false);
  settings_.screensaverTimeoutSec = prefs.getUShort("ssTimeout", 120);
  settings_.audioSampleRateHz = prefs.getUInt("sampleRate", 16000);
  settings_.micGainDb = prefs.getFloat("micGain", 24.0f);
  settings_.speakerVolumeDb = prefs.getFloat("volume", 20.0f);
  settings_.wallpaperChoice = (int8_t)prefs.getChar("wallpaper", -1);
  settings_.showTempHumidity = prefs.getBool("showTempHum", true);
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
  strncpy(settings_.sttEndpoint, endpoint, sizeof(settings_.sttEndpoint) - 1);
  strncpy(settings_.sttModel, model, sizeof(settings_.sttModel) - 1);
  strncpy(settings_.sttApiKey, apiKey, sizeof(settings_.sttApiKey) - 1);
  prefs.putString("sttEndpoint", settings_.sttEndpoint);
  prefs.putString("sttModel", settings_.sttModel);
  prefs.putString("sttApiKey", settings_.sttApiKey);
  return true;
}

bool SettingsStore::saveDriveApp(const char *clientId, const char *clientSecret) {
  strncpy(settings_.driveClientId, clientId, sizeof(settings_.driveClientId) - 1);
  strncpy(settings_.driveClientSecret, clientSecret, sizeof(settings_.driveClientSecret) - 1);
  prefs.putString("driveCliId", settings_.driveClientId);
  prefs.putString("driveCliSec", settings_.driveClientSecret);
  return true;
}

bool SettingsStore::saveDriveRefreshToken(const char *refreshToken) {
  strncpy(settings_.driveRefreshToken, refreshToken, sizeof(settings_.driveRefreshToken) - 1);
  prefs.putString("driveRefTok", settings_.driveRefreshToken);
  return true;
}

bool SettingsStore::saveDriveFolderId(const char *folderId) {
  strncpy(settings_.driveFolderId, folderId, sizeof(settings_.driveFolderId) - 1);
  prefs.putString("driveFolder", settings_.driveFolderId);
  return true;
}

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
