#pragma once
#include <Arduino.h>
#include "settings.h"

// Autenticacao (fluxo OAuth 2.0 "device code" para TV/dispositivos de
// entrada limitada - https://developers.google.com/identity/protocols/oauth2/limited-input-device)
// e upload para uma pasta propria no Google Drive via a API v3, usando
// so o escopo drive.file (o app so enxerga o que ele mesmo cria).
class GDriveClient {
public:
  // Roda o fluxo completo: pede o device code, mostra o codigo do
  // usuario e a URL numa callback (pra desenhar na tela), e fica
  // esperando (poll) ate o usuario aprovar em outro aparelho ou o
  // codigo expirar. Salva o refresh_token em SettingsStore ao terminar.
  using ShowCodeFn = void (*)(const char *userCode, const char *verificationUrl);
  bool pairDevice(SettingsStore &settings, ShowCodeFn showCode);

  // Envia wavPath (e txtPath, se != nullptr) para a pasta do app no
  // Drive, criando a pasta na primeira vez. Renova o access token
  // sozinho a partir do refresh_token salvo.
  bool uploadNote(SettingsStore &settings, const char *wavPath, const char *txtPath);

private:
  bool refreshAccessToken(Settings &cfg, String &outAccessToken);
  bool ensureFolder(Settings &cfg, const String &accessToken, String &outFolderId);
  bool uploadFile(const String &accessToken, const String &folderId, const char *localPath,
                   const char *driveName, const char *mimeType);
};
