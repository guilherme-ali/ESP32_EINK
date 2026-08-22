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
  prefs.getString("wifiSsid", settings_.wifiSsid, sizeof(settings_.wifiSsid));
  prefs.getString("wifiPass", settings_.wifiPass, sizeof(settings_.wifiPass));
  prefs.getString("sttEndpoint", settings_.sttEndpoint, sizeof(settings_.sttEndpoint));
  String model = prefs.getString("sttModel", settings_.sttModel);
  strncpy(settings_.sttModel, model.c_str(), sizeof(settings_.sttModel) - 1);
  prefs.getString("sttApiKey", settings_.sttApiKey, sizeof(settings_.sttApiKey));
  prefs.getString("driveCliId", settings_.driveClientId, sizeof(settings_.driveClientId));
  prefs.getString("driveCliSec", settings_.driveClientSecret, sizeof(settings_.driveClientSecret));
  prefs.getString("driveRefTok", settings_.driveRefreshToken, sizeof(settings_.driveRefreshToken));
  prefs.getString("driveFolder", settings_.driveFolderId, sizeof(settings_.driveFolderId));
  settings_.autoSyncEnabled = prefs.getBool("autoSync", true);
  settings_.screensaverTimeoutSec = prefs.getUShort("ssTimeout", 120);
  settings_.audioSampleRateHz = prefs.getUInt("sampleRate", 16000);
  settings_.micGainDb = prefs.getFloat("micGain", 24.0f);
  settings_.lockRefreshSec = prefs.getUShort("lockRefresh", 300);
  settings_.wallpaperChoice = (int8_t)prefs.getChar("wallpaper", -1);
  settings_.showTempHumidity = prefs.getBool("showTempHum", true);
}

bool SettingsStore::saveWifi(const char *ssid, const char *pass) {
  strncpy(settings_.wifiSsid, ssid, sizeof(settings_.wifiSsid) - 1);
  strncpy(settings_.wifiPass, pass, sizeof(settings_.wifiPass) - 1);
  prefs.putString("wifiSsid", settings_.wifiSsid);
  prefs.putString("wifiPass", settings_.wifiPass);
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

bool SettingsStore::saveLockRefresh(uint16_t seconds) {
  settings_.lockRefreshSec = seconds;
  prefs.putUShort("lockRefresh", seconds);
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
