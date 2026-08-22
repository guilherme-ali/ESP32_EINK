#pragma once
#include <Arduino.h>

// Todas as configuracoes do dispositivo, persistidas em NVS (namespace
// "cfg"). So Wi-Fi por enquanto (Fase 4); STT e Google Drive entram nas
// Fases 5/6 - os campos ja ficam aqui para nao remexer no schema depois.
struct Settings {
  char wifiSsid[33] = "";
  char wifiPass[65] = "";

  char sttEndpoint[128] = "";
  char sttModel[32] = "whisper-large-v3";
  char sttApiKey[128] = "";

  char driveClientId[128] = "";
  char driveClientSecret[64] = "";
  char driveRefreshToken[256] = "";
  char driveFolderId[64] = ""; // preenchido sozinho na primeira sincronizacao

  bool autoSyncEnabled = true;
  uint16_t screensaverTimeoutSec = 120;
};

class SettingsStore {
public:
  bool begin();
  Settings &get() { return settings_; }

  bool saveWifi(const char *ssid, const char *pass);
  bool saveStt(const char *endpoint, const char *model, const char *apiKey);
  bool saveDriveApp(const char *clientId, const char *clientSecret);
  bool saveDriveRefreshToken(const char *refreshToken);
  bool saveDriveFolderId(const char *folderId);
  bool hasWifi() const { return settings_.wifiSsid[0] != '\0'; }
  bool hasDriveApp() const { return settings_.driveClientId[0] != '\0'; }
  bool hasDriveAuth() const { return settings_.driveRefreshToken[0] != '\0'; }

private:
  Settings settings_;
  void load();
};
