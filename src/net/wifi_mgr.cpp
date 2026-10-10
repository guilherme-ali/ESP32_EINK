#include "wifi_mgr.h"
#include "../storage/note_files.h"
#include <LittleFS.h>
#include <Preferences.h>

namespace {
void rememberLink(const char *ssid) {
  const uint8_t *bssid = WiFi.BSSID();
  if (!bssid) return;
  char hex[13];
  snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  Preferences prefs;
  if (!prefs.begin("wifiPerf", false)) return;
  if (prefs.getString("ssid", "") != ssid) prefs.putString("ssid", ssid);
  if (prefs.getString("bssid", "") != hex) prefs.putString("bssid", hex);
  if (prefs.getUInt("channel", 0) != static_cast<unsigned>(WiFi.channel())) prefs.putUInt("channel", WiFi.channel());
  prefs.end();
}
}

bool WifiManager::connect(SettingsStore &settings, uint32_t timeoutMs) {
  settings_ = &settings;
  const Settings &cfg = settings.get();
  if (cfg.wifiNetworkCount == 0) return false;

  WiFi.mode(WIFI_STA);
  // Tenta favorita/ultima rede diretamente. O scan completo e o fallback,
  // nao um custo obrigatorio cada vez que o usuario sincroniza uma nota.
  String lastSsid, lastBssid;
  unsigned lastChannel = 0;
  Preferences prefs;
  if (prefs.begin("wifiPerf", true)) {
    lastSsid = prefs.getString("ssid", ""); lastBssid = prefs.getString("bssid", "");
    lastChannel = prefs.getUInt("channel", 0); prefs.end();
  }
  const char *preferred = cfg.favoriteWifiSsid[0] ? cfg.favoriteWifiSsid : lastSsid.c_str();
  for (int i = 0; preferred[0] && i < cfg.wifiNetworkCount; ++i) {
    if (strcmp(cfg.wifiSsid[i], preferred)) continue;
    uint8_t bssid[6]; bool pinned = lastSsid == preferred && lastBssid.length() == 12 && lastChannel >= 1 && lastChannel <= 14;
    if (pinned) {
      for (unsigned byte = 0; byte < 6; ++byte) {
        char pair[3] = {lastBssid[byte * 2], lastBssid[byte * 2 + 1], 0};
        if (!isxdigit(static_cast<unsigned char>(pair[0])) || !isxdigit(static_cast<unsigned char>(pair[1]))) { pinned = false; break; }
        bssid[byte] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
      }
    }
    Serial.printf("[Wi-Fi] Tentativa direta: '%s'%s\n", preferred, pinned ? " (canal/BSSID em cache)" : "");
    WiFi.begin(cfg.wifiSsid[i], cfg.wifiPass[i], pinned ? lastChannel : 0, pinned ? bssid : nullptr);
    uint32_t started = millis(), directTimeout = min(timeoutMs, static_cast<uint32_t>(6000));
    while (WiFi.status() != WL_CONNECTED && millis() - started < directTimeout) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      mode_ = Mode::Station; rememberLink(cfg.wifiSsid[i]); startServerOnce();
      Serial.printf("[Wi-Fi] Conexao direta pronta: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    WiFi.disconnect(false); break;
  }
  int found = WiFi.scanNetworks();

  // Encontra todas as redes salvas visiveis e seus melhores sinais.
  struct VisibleSaved {
    int configIdx;
    int rssi;
    bool isFavorite;
  };
  VisibleSaved candidates[Settings::kMaxWifiNetworks];
  int candidateCount = 0;

  for (int i = 0; i < cfg.wifiNetworkCount; i++) {
    int bestRssi = -1000;
    bool visible = false;
    for (int j = 0; j < found; j++) {
      if (WiFi.SSID(j) == cfg.wifiSsid[i]) {
        visible = true;
        if (WiFi.RSSI(j) > bestRssi) bestRssi = WiFi.RSSI(j);
      }
    }
    if (visible) {
      bool isFav = (cfg.favoriteWifiSsid[0] != '\0' &&
                    strcmp(cfg.wifiSsid[i], cfg.favoriteWifiSsid) == 0);
      candidates[candidateCount++] = {i, bestRssi, isFav};
    }
  }
  WiFi.scanDelete();

  if (candidateCount == 0) {
    Serial.println("[Wi-Fi] Nenhuma rede salva por perto.");
    WiFi.mode(WIFI_OFF);
    mode_ = Mode::Off;
    return false;
  }

  // Ordena: favorita primeiro; depois por sinal (RSSI) decrescente.
  for (int i = 0; i < candidateCount - 1; i++) {
    for (int j = i + 1; j < candidateCount; j++) {
      bool swap = false;
      if (!candidates[i].isFavorite && candidates[j].isFavorite) {
        swap = true;
      } else if (candidates[i].isFavorite == candidates[j].isFavorite &&
                 candidates[j].rssi > candidates[i].rssi) {
        swap = true;
      }
      if (swap) {
        VisibleSaved tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
      }
    }
  }

  // Tenta cada rede visivel em ordem de preferencia.
  for (int c = 0; c < candidateCount; c++) {
    int idx = candidates[c].configIdx;
    Serial.printf("[Wi-Fi] Conectando a '%s'%s (RSSI %d)...\n",
                  cfg.wifiSsid[idx], candidates[c].isFavorite ? " (favorita)" : "",
                  candidates[c].rssi);

    WiFi.disconnect(false);
    WiFi.begin(cfg.wifiSsid[idx], cfg.wifiPass[idx]);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      mode_ = Mode::Station;
      rememberLink(cfg.wifiSsid[idx]);
      Serial.printf("[Wi-Fi] Conectado a '%s': %s\n", cfg.wifiSsid[idx],
                    WiFi.localIP().toString().c_str());
      startServerOnce();
      return true;
    }
    Serial.printf("[Wi-Fi] Falha ao conectar em '%s'.\n", cfg.wifiSsid[idx]);
  }

  Serial.println("[Wi-Fi] Nenhuma rede conectou.");
  WiFi.mode(WIFI_OFF);
  mode_ = Mode::Off;
  return false;
}

void WifiManager::disconnect() {
  if (serverStarted_) { server_.stop(); serverStarted_ = false; }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mode_ = Mode::Off;
  Serial.println("Wi-Fi desligado.");
}

void WifiManager::startApPortal(SettingsStore &settings) {
  settings_ = &settings;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[24];
  snprintf(name, sizeof(name), "IdeiaRec-%02X%02X", mac[4], mac[5]);
  apName_ = name;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName_.c_str());

  mode_ = Mode::ApPortal;
  Serial.printf("Portal AP '%s' em %s\n", apName_.c_str(), WiFi.softAPIP().toString().c_str());
  startServerOnce();
}

void WifiManager::startServerOnce() {
  if (serverStarted_) return;
  if (!routesRegistered_) {
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/notes", HTTP_GET, [this]() { handleListNotes(); });
  server_.on("/note", HTTP_GET, [this]() { handleGetNote(); });
  routesRegistered_ = true;
  }
  server_.begin();
  serverStarted_ = true;
}

int WifiManager::scan() {
  if (mode_ == Mode::Off) WiFi.mode(WIFI_STA);
  return WiFi.scanNetworks();
}

String WifiManager::scanSsid(int i) const { return WiFi.SSID(i); }
int WifiManager::scanRssi(int i) const { return WiFi.RSSI(i); }

// GET /notes - lista em texto simples os arquivos de /notes, um por
// linha, com tamanho em bytes. Usado pra achar o nome antes de baixar
// com /note?name=... (nao existe UI pra isso - e ferramenta de debug).
void WifiManager::handleListNotes() {
  File dir = LittleFS.open("/notes");
  if (!dir || !dir.isDirectory()) {
    server_.send(404, "text/plain", "sem /notes");
    return;
  }
  String out;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      size_t size = f.size();
      if (name.endsWith(".wav")) {
        File audio = NoteFiles::openRead("/notes/" + name);
        if (audio) size = audio.size();
      }
      out += name + "\t" + String((unsigned long)size) + "\n";
    }
    f = dir.openNextFile();
  }
  server_.send(200, "text/plain", out);
}

// GET /note?name=ARQUIVO.wav - baixa o arquivo bruto de /notes. So pra
// diagnostico (baixar e ouvir/analisar no PC) - sem essa rota nao tem
// como tirar um WAV do dispositivo sem sincronizar pro Drive.
void WifiManager::handleGetNote() {
  if (!server_.hasArg("name")) {
    server_.send(400, "text/plain", "falta ?name=");
    return;
  }
  String name = server_.arg("name");
  if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
    server_.send(400, "text/plain", "nome invalido");
    return;
  }
  String path = "/notes/" + name;
  File f = NoteFiles::openRead(path);
  if (!f) {
    server_.send(404, "text/plain", "nao encontrado");
    return;
  }
  server_.streamFile(f, "application/octet-stream");
  f.close();
}

void WifiManager::handleRoot() {
  const Settings &cfg = settings_->get();

  String html;
  html.reserve(1800);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Gravador de Ideias - Config</title><style>"
             "body{font-family:sans-serif;max-width:380px;margin:24px auto;padding:0 12px}"
             "input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box}"
             "button{width:100%;padding:10px;background:#222;color:#fff;border:0;border-radius:4px}"
             "h3{margin-top:28px;border-top:1px solid #ccc;padding-top:12px}"
             "ul{padding-left:18px;margin:4px 0}"
             "</style></head><body><form action='/save' method='POST'>");

  html += F("<h2>Configurar dispositivo</h2><h3>Wi-Fi</h3>");
  if (cfg.wifiNetworkCount > 0) {
    html += F("<p>Redes salvas:</p><ul>");
    for (int i = 0; i < cfg.wifiNetworkCount; i++) {
      html += "<li>" + String(cfg.wifiSsid[i]) + "</li>";
    }
    html += F("</ul>");
  } else {
    html += F("<p>Nenhuma rede salva ainda.</p>");
  }
  html += F("<label>Adicionar/atualizar rede (SSID)</label><input name='ssid'>"
             "<label>Senha (deixe em branco p/ manter, se ja existir)</label>"
             "<input name='pass' type='password'>");

  html += F("<h3>Transcricao (STT)</h3>"
             "<label>Endpoint (URL da API)</label><input name='stt_endpoint' value='");
  html += cfg.sttEndpoint;
  html += F("'><label>Modelo</label><input name='stt_model' value='");
  html += cfg.sttModel;
  html += F("'><label>Modelo do Markdown</label><input name='summary_model' value='");
  html += cfg.summaryModel;
  html += F("'><label><input style='width:auto' type='checkbox' name='ai_auto' value='1'");
  if (cfg.sttAutoModel) html += F(" checked");
  html += F("> Escolha automatica: Transcribe / Flash / Flash-Lite</label>"
            "<p>Transcricao: 3.5 Transcribe, 3.8 Flash, 3.5 Flash-Lite. Markdown: 3.8 Flash, 3.5 Flash-Lite.</p>"
            "<label><input style='width:auto' type='checkbox' name='free_confirmed' value='1'");
  if (cfg.geminiFreeConfirmed) html += F(" checked");
  html += F("> Confirmo que esta chave pertence a projeto gratuito, sem faturamento pago</label>"
            "<p>A API nao informa o plano de faturamento pela lista de modelos. Quando a cota acabar, a nota fica pendente.</p>"
            "<label>API key (deixe em branco p/ manter)</label>"
              "<input name='stt_key' type='password'>");

  html += F("<h3>Google Drive</h3>"
             "<label>Client ID</label><input name='drive_id' value='");
  html += cfg.driveClientId;
  html += F("'><label>Client secret (deixe em branco p/ manter)</label>"
              "<input name='drive_secret' type='password'>"
              "<label><input style='width:auto' type='checkbox' name='drive_repair' value='1'> Refazer autorizacao do Drive ao conectar</label>");

  html += F("<button type='submit'>Salvar</button></form></body></html>");

  server_.send(200, "text/html", html);
}

void WifiManager::handleSave() {
  String ssid = server_.arg("ssid");
  String pass = server_.arg("pass");
  String sttEndpoint = server_.arg("stt_endpoint");
  String sttModel = server_.arg("stt_model");
  String sttKey = server_.arg("stt_key");
  String driveId = server_.arg("drive_id");
  String driveSecret = server_.arg("drive_secret");

  Settings &cfg = settings_->get();
  if (ssid.length() > 0) {
    String finalPass = pass;
    if (finalPass.length() == 0) {
      for (int i = 0; i < cfg.wifiNetworkCount; i++) {
        if (ssid == cfg.wifiSsid[i]) {
          finalPass = cfg.wifiPass[i];
          break;
        }
      }
    }
    settings_->saveWifiNetwork(ssid.c_str(), finalPass.c_str());
  }
  String summaryModel = server_.arg("summary_model");
  if (!summaryModel.length()) summaryModel = cfg.summaryModel;
  bool ok = settings_->saveStt(sttEndpoint.c_str(), sttModel.c_str(),
                       sttKey.length() > 0 ? sttKey.c_str() : cfg.sttApiKey);
  ok = settings_->saveAiPolicy(server_.hasArg("ai_auto"), summaryModel.c_str(),
                               server_.hasArg("free_confirmed")) && ok;
  ok = settings_->saveDriveApp(driveId.c_str(),
                            driveSecret.length() > 0 ? driveSecret.c_str() : cfg.driveClientSecret) && ok;
  if (server_.hasArg("drive_repair")) ok = settings_->saveDriveRefreshToken("") && ok;
  if (!ok) { server_.send(400, "text/plain", "Configuracao invalida ou nao foi possivel salvar."); return; }

  server_.send(200, "text/html",
               "<html><body><h3>Salvo. Reiniciando...</h3></body></html>");
  delay(1000);
  ESP.restart();
}

void WifiManager::loop() {
  if (serverStarted_) server_.handleClient();
}

String WifiManager::statusLine() const {
  if (mode_ == Mode::Station && WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (mode_ == Mode::ApPortal) {
    return apName_ + " " + WiFi.softAPIP().toString();
  }
  return "desligado";
}
