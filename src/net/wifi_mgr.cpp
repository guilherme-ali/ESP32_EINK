#include "wifi_mgr.h"
#include <LittleFS.h>

bool WifiManager::connect(SettingsStore &settings, uint32_t timeoutMs) {
  settings_ = &settings;
  const Settings &cfg = settings.get();
  if (cfg.wifiNetworkCount == 0) return false;

  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();

  // Escaneia uma vez e compara contra TODAS as redes salvas (equivale a
  // "tentar" cada uma, sem gastar bateria com timeouts de conexao em
  // redes fora de alcance). A favorita, se estiver por perto, vence
  // mesmo sem ser a de sinal mais forte.
  int bestIdx = -1;
  int bestRssi = -1000;
  int favoriteIdx = -1;
  for (int i = 0; i < cfg.wifiNetworkCount; i++) {
    for (int j = 0; j < found; j++) {
      if (WiFi.SSID(j) != cfg.wifiSsid[i]) continue;
      if (WiFi.RSSI(j) > bestRssi) {
        bestRssi = WiFi.RSSI(j);
        bestIdx = i;
      }
      if (cfg.favoriteWifiSsid[0] != '\0' && strcmp(cfg.wifiSsid[i], cfg.favoriteWifiSsid) == 0) {
        favoriteIdx = i;
      }
    }
  }
  WiFi.scanDelete();

  int chosen = favoriteIdx >= 0 ? favoriteIdx : bestIdx;
  if (chosen < 0) {
    Serial.println("Nenhuma rede salva por perto.");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.printf("Conectando a '%s'%s...\n", cfg.wifiSsid[chosen],
                chosen == favoriteIdx ? " (favorita)" : "");
  WiFi.begin(cfg.wifiSsid[chosen], cfg.wifiPass[chosen]);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Falha ao conectar.");
    WiFi.mode(WIFI_OFF);
    mode_ = Mode::Off;
    return false;
  }

  mode_ = Mode::Station;
  Serial.printf("Wi-Fi conectado: %s\n", WiFi.localIP().toString().c_str());
  startServerOnce();
  return true;
}

void WifiManager::disconnect() {
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
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/notes", HTTP_GET, [this]() { handleListNotes(); });
  server_.on("/note", HTTP_GET, [this]() { handleGetNote(); });
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
      out += name + "\t" + String((unsigned long)f.size()) + "\n";
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
  File f = LittleFS.open(path, FILE_READ);
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
  html += F("'><label>API key (deixe em branco p/ manter)</label>"
             "<input name='stt_key' type='password'>");

  html += F("<h3>Google Drive</h3>"
             "<label>Client ID</label><input name='drive_id' value='");
  html += cfg.driveClientId;
  html += F("'><label>Client secret (deixe em branco p/ manter)</label>"
             "<input name='drive_secret' type='password'>");

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
  settings_->saveStt(sttEndpoint.c_str(), sttModel.c_str(),
                      sttKey.length() > 0 ? sttKey.c_str() : cfg.sttApiKey);
  settings_->saveDriveApp(driveId.c_str(),
                           driveSecret.length() > 0 ? driveSecret.c_str() : cfg.driveClientSecret);

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
