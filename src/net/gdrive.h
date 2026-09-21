#pragma once
#include <Arduino.h>
#include "settings.h"

#pragma once
#include <Arduino.h>
#include "settings.h"

enum class SyncStage {
  Idle,
  Authenticating,
  CheckingFolder,
  UploadingWav,
  UploadingTxt,
  UploadingMd,
  VerifyingChecksum,
  Done,
  Error
};

struct SyncProgress {
  SyncStage stage = SyncStage::Idle;
  const char *fileName = "";
  size_t bytesUploaded = 0;
  size_t totalBytes = 0;
  int attempt = 0;
  int maxAttempts = 4;
  const char *message = "";
};

using SyncProgressFn = void (*)(const SyncProgress &progress);

struct NoteSyncState {
  static constexpr uint8_t kCurrentVersion = 1;
  uint8_t version = kCurrentVersion;

  bool wavUploaded = false;
  bool txtUploaded = false;
  bool mdUploaded = false;
  bool fullySynced = false;

  char wavDriveId[48] = "";
  char txtDriveId[48] = "";
  char mdDriveId[48] = "";

  char wavSessionUrl[256] = "";
  size_t wavBytesUploaded = 0;

  int lastStatusCode = 0;
  char lastError[64] = "";
  uint32_t lastAttemptTime = 0;

  static String statePathFor(const char *wavPath);
  static String legacySncPathFor(const char *wavPath);

  bool load(const char *wavPath);
  bool save(const char *wavPath) const;
};

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

  // Envia wavPath (e txtPath / mdPath, se existirem) para a pasta do app no
  // Drive, criando a pasta na primeira vez. Renova o access token
  // sozinho a partir do refresh_token salvo. Suporta upload retomavel e
  // salva progresso em NoteSyncState.
  bool uploadNote(SettingsStore &settings, const char *wavPath,
                  const char *txtPath = nullptr, const char *mdPath = nullptr,
                  SyncProgressFn onProgress = nullptr);

  // Consulta estado de sincronizacao de uma nota.
  static bool getNoteSyncState(const char *wavPath, NoteSyncState &outState);

private:
  bool refreshAccessToken(Settings &cfg, String &outAccessToken);
  bool ensureFolder(Settings &cfg, const String &accessToken, String &outFolderId);
  bool uploadFileResumable(const String &accessToken, const String &folderId,
                          const char *localPath, const char *driveName,
                          const char *mimeType, char *outDriveId, size_t outDriveIdLen,
                          char *sessionUrlBuf, size_t sessionUrlBufLen,
                          size_t &confirmedBytes, SyncProgressFn onProgress,
                          SyncStage stage);
};
