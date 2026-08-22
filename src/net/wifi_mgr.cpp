#include "wifi_mgr.h"

bool WifiManager::begin(SettingsStore &settings) {
  settings_ = &settings;
  Settings &cfg = settings.get();

  if (settings.hasWifi()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    Serial.printf("Conectando a '%s'...\n", cfg.wifiSsid);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      mode_ = Mode::Station;
      Serial.printf("Wi-Fi conectado: %s\n", WiFi.localIP().toString().c_str());
      startServer();
      return true;
    }
    Serial.println("Falha ao conectar - abrindo portal de configuracao.");
  }

  startApPortal();
  return false;
}

void WifiManager::startApPortal() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[24];
  snprintf(name, sizeof(name), "IdeiaRec-%02X%02X", mac[4], mac[5]);
  apName_ = name;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName_.c_str());

  mode_ = Mode::ApPortal;
  Serial.printf("Portal AP '%s' em %s\n", apName_.c_str(), WiFi.softAPIP().toString().c_str());
  startServer();
}

void WifiManager::startServer() {
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.begin();
}

void WifiManager::handleRoot() {
  const Settings &cfg = settings_->get();

  String html;
  html.reserve(1600);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Gravador de Ideias - Config</title><style>"
             "body{font-family:sans-serif;max-width:380px;margin:24px auto;padding:0 12px}"
             "input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box}"
             "button{width:100%;padding:10px;background:#222;color:#fff;border:0;border-radius:4px}"
             "h3{margin-top:28px;border-top:1px solid #ccc;padding-top:12px}"
             "</style></head><body><form action='/save' method='POST'>");

  html += F("<h2>Configurar dispositivo</h2><h3>Wi-Fi</h3>"
             "<label>Rede (SSID)</label><input name='ssid' value='");
  html += cfg.wifiSsid;
  html += F("'><label>Senha (deixe em branco p/ manter)</label>"
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
    settings_->saveWifi(ssid.c_str(), pass.length() > 0 ? pass.c_str() : cfg.wifiPass);
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
  server_.handleClient();
}

String WifiManager::statusLine() const {
  if (mode_ == Mode::Station && WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (mode_ == Mode::ApPortal) {
    return apName_ + " " + WiFi.softAPIP().toString();
  }
  return "desconectado";
}
