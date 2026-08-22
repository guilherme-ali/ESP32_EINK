#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "settings.h"

// Wi-Fi fica desligado por padrao (WIFI_OFF) - o radio so liga quando
// algo pede explicitamente: sincronizar, o menu Wi-Fi, ou (so no
// primeiro uso, sem nenhuma rede/config salva) o portal automatico do
// boot. Ver App::ensureOnline() e main.cpp (onConnectRequested/
// onPortalRequested) para quem chama isso.
class WifiManager {
public:
  enum class Mode { Off, Station, ApPortal };

  // Escaneia as redes por perto e conecta na rede salva mais forte que
  // estiver visivel (evita esperar o timeout completo em redes salvas
  // que nao estao por perto). Bloqueante (alguns segundos).
  bool connect(SettingsStore &settings, uint32_t timeoutMs = 12000);
  void disconnect(); // desliga o radio (WIFI_OFF)

  // Sobe um Access Point proprio ("IdeiaRec-XXXX") com portal web em
  // 192.168.4.1 para configurar Wi-Fi/STT/Drive sem o teclado na tela.
  // Quem chama e responsavel pelo loop() e por decidir quando sair
  // (ver onPortalRequested() em main.cpp).
  void startApPortal(SettingsStore &settings);

  void loop(); // chamar sempre que o servidor web puder estar ativo

  Mode mode() const { return mode_; }
  bool isConnected() const { return mode_ == Mode::Station && WiFi.status() == WL_CONNECTED; }

  String statusLine() const;
  String apName() const { return apName_; }

  // Escaneia redes por perto (bloqueante, poucos segundos) - usado pelo
  // menu Wi-Fi para "adicionar por escaneamento" sem digitar o SSID.
  int scan();
  String scanSsid(int i) const;
  int scanRssi(int i) const;

private:
  SettingsStore *settings_ = nullptr;
  WebServer server_{80};
  Mode mode_ = Mode::Off;
  String apName_;
  bool serverStarted_ = false;

  void startServerOnce();
  void handleRoot();
  void handleSave();
  void handleListNotes();
  void handleGetNote();
};
