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
  static constexpr uint8_t kCurrentVersion = 3;
  uint8_t version = kCurrentVersion;
  uint32_t authGeneration = 0;

  bool wavUploaded = false;
  bool txtUploaded = false;
  bool mdUploaded = false;
  bool fullySynced = false;

  // Um ID pode estar apenas reservado: somente Uploaded + hash verificado
  // representa sucesso. Strings de sessao nunca sao truncadas.
  char wavDriveId[128] = "";
  char txtDriveId[128] = "";
  char mdDriveId[128] = "";
  bool wavIdReserved = false;
  bool txtIdReserved = false;
  bool mdIdReserved = false;
  char folderId[128] = "";
  char pendingFolderId[128] = ""; // reserva antes do POST da pasta
  String wavSessionUrl;
  String txtSessionUrl;
  String mdSessionUrl;
  size_t wavBytesUploaded = 0;
  size_t txtBytesUploaded = 0;
  size_t mdBytesUploaded = 0;
  char wavMd5[33] = "";
  char txtMd5[33] = "";
  char mdMd5[33] = "";
  size_t wavSize = 0;
  size_t txtSize = 0;
  size_t mdSize = 0;

  int lastStatusCode = 0;
  char lastError[384] = "";
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
  // service() atende a UI durante a espera; false cancela. Nao e chamada
  // dentro das operacoes TLS bloqueantes (limitadas pelos timeouts HTTP).
  using ServiceFn = bool (*)();
  bool pairDevice(SettingsStore &settings, ShowCodeFn showCode,
                  ServiceFn service = nullptr);

  // Envia wavPath (e txtPath / mdPath, se existirem) para a pasta do app no
  // Drive, criando a pasta na primeira vez. Renova o access token
  // sozinho a partir do refresh_token salvo. Suporta upload retomavel e
  // salva progresso em NoteSyncState.
  bool uploadNote(SettingsStore &settings, const char *wavPath,
                  const char *txtPath = nullptr, const char *mdPath = nullptr,
                  SyncProgressFn onProgress = nullptr);

  // Consulta estado de sincronizacao de uma nota.
  static bool getNoteSyncState(const char *wavPath, NoteSyncState &outState);
  // Consulta local conservadora: arquivos atuais, IDs, tamanhos e hashes.
  // requireText exige os dois irmaos .txt/.md validos (STT configurado).
  static bool needsUpload(const char *wavPath, bool requireText);

  const String &lastError() const { return lastError_; }
  int lastStatusCode() const { return lastStatusCode_; }

private:
  struct Job;
  String lastError_;
  int lastStatusCode_ = 0;
  bool fail(const String &message, int statusCode);
  bool refreshAccessToken(Settings &cfg, String &outAccessToken);
};
