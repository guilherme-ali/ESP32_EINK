#pragma once
#include <Arduino.h>

// Todas as configuracoes do dispositivo, persistidas em NVS (namespace
// "cfg"). So Wi-Fi por enquanto (Fase 4); STT e Google Drive entram nas
// Fases 5/6 - os campos ja ficam aqui para nao remexer no schema depois.
struct Settings {
  // Ate 5 redes salvas (Fase D) - o dispositivo escaneia e conecta na
  // que estiver por perto, sob demanda (Sincronizar ou menu Wi-Fi), em
  // vez de manter o radio ligado o tempo todo.
  static constexpr int kMaxWifiNetworks = 5;
  char wifiSsid[kMaxWifiNetworks][33] = {};
  char wifiPass[kMaxWifiNetworks][65] = {};
  uint8_t wifiNetworkCount = 0;
  // Se estiver por perto na hora de conectar, essa rede tem prioridade
  // sobre a de sinal mais forte. "" = sem favorita (so usa sinal).
  char favoriteWifiSsid[33] = "";

  char sttEndpoint[128] = "";
  char sttModel[32] = "whisper-large-v3";
  char sttApiKey[128] = "";

  char driveClientId[128] = "";
  char driveClientSecret[64] = "";
  char driveRefreshToken[256] = "";
  char driveFolderId[64] = ""; // preenchido sozinho na primeira sincronizacao

  bool autoSyncEnabled = false; // padrao: nao transcreve/envia sozinho apos gravar, so via "Sincronizar"
  uint16_t screensaverTimeoutSec = 120;

  // 8000/16000/32000/44100/48000 - precisa ter entrada na tabela de
  // coeficientes de clock do ES8311 (lib/es8311/es8311.c).
  uint32_t audioSampleRateHz = 16000;
  float micGainDb = 24.0f; // 0-42dB, ver es8311_mic_gain_t
  float speakerVolumeDb = 20.0f;

  uint16_t lockRefreshSec = 300; // intervalo do relogio na tela de bloqueio
  int8_t wallpaperChoice = -1;   // -1 = sorteia a cada vez que dorme; N = fixo
  bool showTempHumidity = true;
};

class SettingsStore {
public:
  bool begin();
  Settings &get() { return settings_; }

  // Adiciona uma rede nova (se houver vaga) ou atualiza a senha se o
  // SSID ja estiver salvo. Retorna false so quando ja tem 5 redes E o
  // SSID e novo (sem vaga).
  bool saveWifiNetwork(const char *ssid, const char *pass);
  bool removeWifiNetwork(int index);
  bool saveFavoriteWifi(const char *ssid); // "" limpa a favorita
  bool saveStt(const char *endpoint, const char *model, const char *apiKey);
  bool saveDriveApp(const char *clientId, const char *clientSecret);
  bool saveDriveRefreshToken(const char *refreshToken);
  bool saveDriveFolderId(const char *folderId);
  bool saveAudio(uint32_t sampleRateHz, float micGainDb);
  bool saveVolume(float volumeDb);
  bool saveScreensaverTimeout(uint16_t seconds);
  bool saveAutoSync(bool enabled);
  bool saveLockRefresh(uint16_t seconds);
  bool saveWallpaperChoice(int8_t choice);
  bool saveShowTempHumidity(bool enabled);
  bool hasWifi() const { return settings_.wifiNetworkCount > 0; }
  bool hasDriveApp() const { return settings_.driveClientId[0] != '\0'; }
  bool hasDriveAuth() const { return settings_.driveRefreshToken[0] != '\0'; }

private:
  Settings settings_;
  void load();
};
