#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "settings.h"

// Conecta ao Wi-Fi salvo; se nao houver credenciais ou a conexao falhar,
// sobe um Access Point proprio ("IdeiaRec-XXXX") com um portal web em
// 192.168.4.1. O mesmo servidor web continua no ar depois de conectado
// (agora no IP da rede local) servindo a mesma pagina de configuracoes
// (Wi-Fi + transcricao) - assim da para reconfigurar sem precisar
// voltar ao modo AP.
class WifiManager {
public:
  enum class Mode { Connecting, Station, ApPortal };

  bool begin(SettingsStore &settings);
  void loop(); // chamar sempre, em qualquer modo

  Mode mode() const { return mode_; }
  bool isConnected() const { return mode_ == Mode::Station && WiFi.status() == WL_CONNECTED; }

  // Texto pronto para a tela: IP quando conectado, ou "SSID / IP" do AP.
  String statusLine() const;
  String apName() const { return apName_; }

private:
  SettingsStore *settings_ = nullptr;
  WebServer server_{80};
  Mode mode_ = Mode::Connecting;
  String apName_;

  void startApPortal();
  void startServer();
  void handleRoot();
  void handleSave();
  void handleListNotes();
  void handleGetNote();
};
