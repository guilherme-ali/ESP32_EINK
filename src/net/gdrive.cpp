#include "gdrive.h"
#include "http_client.h"
#include "json_utils.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <MD5Builder.h>
#include <cJSON.h>

namespace {

String urlEncode(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

String calculateFileMd5(const char *path) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return "";
  MD5Builder md5;
  md5.begin();
  md5.addStream(f, f.size());
  md5.calculate();
  f.close();
  return md5.toString();
}

bool httpsPostSmall(const String &host, const String &path, const String &contentType,
                     const String &authHeader, const String &body, HttpResponse &outResp,
                     uint32_t timeoutMs = 20000) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(timeoutMs);

  if (!client.connect(host.c_str(), 443)) {
    Serial.printf("[Drive] falha ao conectar em %s:443\n", host.c_str());
    return false;
  }

  String headers = "POST " + path + " HTTP/1.1\r\n" +
                   "Host: " + host + "\r\n";
  if (authHeader.length() > 0) {
    headers += authHeader + "\r\n";
  }
  headers += "Content-Type: " + contentType + "\r\n" +
             "Content-Length: " + String((unsigned)body.length()) + "\r\n" +
             "Connection: close\r\n\r\n";

  if (!HttpClient::writeAll(client, headers, timeoutMs)) {
    client.stop();
    return false;
  }
  if (!HttpClient::writeAll(client, body, timeoutMs)) {
    client.stop();
    return false;
  }

  return HttpClient::readResponse(client, outResp, HttpClient::kMaxResponseBody, timeoutMs);
}

} // namespace

String NoteSyncState::statePathFor(const char *wavPath) {
  String p = String(wavPath);
  p.replace(".wav", ".sync");
  return p;
}

String NoteSyncState::legacySncPathFor(const char *wavPath) {
  String p = String(wavPath);
  p.replace(".wav", ".snc");
  return p;
}

bool NoteSyncState::load(const char *wavPath) {
  String path = statePathFor(wavPath);
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, FILE_READ);
    if (f) {
      String json = f.readString();
      f.close();
      cJSON *root = cJSON_Parse(json.c_str());
      if (root) {
        cJSON *v = cJSON_GetObjectItem(root, "version");
        if (v && cJSON_IsNumber(v)) version = (uint8_t)v->valueint;

        cJSON *w = cJSON_GetObjectItem(root, "wavUploaded");
        if (w && cJSON_IsBool(w)) wavUploaded = cJSON_IsTrue(w);

        cJSON *t = cJSON_GetObjectItem(root, "txtUploaded");
        if (t && cJSON_IsBool(t)) txtUploaded = cJSON_IsTrue(t);

        cJSON *m = cJSON_GetObjectItem(root, "mdUploaded");
        if (m && cJSON_IsBool(m)) mdUploaded = cJSON_IsTrue(m);

        cJSON *fs = cJSON_GetObjectItem(root, "fullySynced");
        if (fs && cJSON_IsBool(fs)) fullySynced = cJSON_IsTrue(fs);

        cJSON *wid = cJSON_GetObjectItem(root, "wavDriveId");
        if (wid && cJSON_IsString(wid) && wid->valuestring) {
          strncpy(wavDriveId, wid->valuestring, sizeof(wavDriveId) - 1);
        }
        cJSON *tid = cJSON_GetObjectItem(root, "txtDriveId");
        if (tid && cJSON_IsString(tid) && tid->valuestring) {
          strncpy(txtDriveId, tid->valuestring, sizeof(txtDriveId) - 1);
        }
        cJSON *mid = cJSON_GetObjectItem(root, "mdDriveId");
        if (mid && cJSON_IsString(mid) && mid->valuestring) {
          strncpy(mdDriveId, mid->valuestring, sizeof(mdDriveId) - 1);
        }
        cJSON *surl = cJSON_GetObjectItem(root, "wavSessionUrl");
        if (surl && cJSON_IsString(surl) && surl->valuestring) {
          strncpy(wavSessionUrl, surl->valuestring, sizeof(wavSessionUrl) - 1);
        }
        cJSON *bu = cJSON_GetObjectItem(root, "wavBytesUploaded");
        if (bu && cJSON_IsNumber(bu)) wavBytesUploaded = (size_t)bu->valuedouble;

        cJSON *sc = cJSON_GetObjectItem(root, "lastStatusCode");
        if (sc && cJSON_IsNumber(sc)) lastStatusCode = sc->valueint;

        cJSON *le = cJSON_GetObjectItem(root, "lastError");
        if (le && cJSON_IsString(le) && le->valuestring) {
          strncpy(lastError, le->valuestring, sizeof(lastError) - 1);
        }
        cJSON *lat = cJSON_GetObjectItem(root, "lastAttemptTime");
        if (lat && cJSON_IsNumber(lat)) lastAttemptTime = (uint32_t)lat->valuedouble;

        cJSON_Delete(root);
        return true;
      }
    }
  }

  // Migracao do legado .snc se existir
  String legacyPath = legacySncPathFor(wavPath);
  if (LittleFS.exists(legacyPath)) {
    wavUploaded = true;
    String txtP = String(wavPath); txtP.replace(".wav", ".txt");
    String mdP = String(wavPath); mdP.replace(".wav", ".md");
    txtUploaded = LittleFS.exists(txtP);
    mdUploaded = LittleFS.exists(mdP);
    fullySynced = true;
    save(wavPath);
    return true;
  }

  return false;
}

bool NoteSyncState::save(const char *wavPath) const {
  cJSON *root = cJSON_CreateObject();
  if (!root) return false;

  cJSON_AddNumberToObject(root, "version", version);
  cJSON_AddBoolToObject(root, "wavUploaded", wavUploaded);
  cJSON_AddBoolToObject(root, "txtUploaded", txtUploaded);
  cJSON_AddBoolToObject(root, "mdUploaded", mdUploaded);
  cJSON_AddBoolToObject(root, "fullySynced", fullySynced);
  cJSON_AddStringToObject(root, "wavDriveId", wavDriveId);
  cJSON_AddStringToObject(root, "txtDriveId", txtDriveId);
  cJSON_AddStringToObject(root, "mdDriveId", mdDriveId);
  cJSON_AddStringToObject(root, "wavSessionUrl", wavSessionUrl);
  cJSON_AddNumberToObject(root, "wavBytesUploaded", (double)wavBytesUploaded);
  cJSON_AddNumberToObject(root, "lastStatusCode", lastStatusCode);
  cJSON_AddStringToObject(root, "lastError", lastError);
  cJSON_AddNumberToObject(root, "lastAttemptTime", lastAttemptTime);

  char *rendered = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!rendered) return false;

  String tmpPath = statePathFor(wavPath) + ".tmp";
  String finalPath = statePathFor(wavPath);

  File f = LittleFS.open(tmpPath, FILE_WRITE);
  if (!f) {
    free(rendered);
    return false;
  }
  f.print(rendered);
  f.close();
  free(rendered);

  if (LittleFS.exists(finalPath)) LittleFS.remove(finalPath);
  bool ok = LittleFS.rename(tmpPath, finalPath);

  // Mantem o marcador legado .snc em sincronia para compatibilidade
  String legacyPath = legacySncPathFor(wavPath);
  if (fullySynced) {
    if (!LittleFS.exists(legacyPath)) {
      File lf = LittleFS.open(legacyPath, FILE_WRITE);
      if (lf) lf.close();
    }
  } else {
    if (LittleFS.exists(legacyPath)) {
      LittleFS.remove(legacyPath);
    }
  }

  return ok;
}

bool GDriveClient::pairDevice(SettingsStore &settingsStore, ShowCodeFn showCode) {
  Settings &cfg = settingsStore.get();
  if (cfg.driveClientId[0] == '\0') {
    Serial.println("[Drive] Client ID nao configurado.");
    return false;
  }

  String reqBody = "client_id=" + urlEncode(cfg.driveClientId) +
                   "&scope=" + urlEncode("https://www.googleapis.com/auth/drive.file");
  HttpResponse resp;
  if (!httpsPostSmall("oauth2.googleapis.com", "/device/code",
                      "application/x-www-form-urlencoded", "", reqBody, resp) ||
      resp.statusCode != 200) {
    Serial.printf("[Drive] falha ao pedir device code (HTTP %d):\n%s\n",
                  resp.statusCode, resp.body.c_str());
    return false;
  }

  char deviceCode[128], userCode[32], verificationUrl[64];
  if (!jsonGetString(resp.body, "device_code", deviceCode, sizeof(deviceCode)) ||
      !jsonGetString(resp.body, "user_code", userCode, sizeof(userCode)) ||
      !jsonGetString(resp.body, "verification_url", verificationUrl, sizeof(verificationUrl))) {
    Serial.println("[Drive] resposta de device_code incompleta:");
    Serial.println(resp.body);
    return false;
  }
  long interval = 5, expiresIn = 1800;
  jsonGetLong(resp.body, "interval", interval);
  jsonGetLong(resp.body, "expires_in", expiresIn);

  Serial.printf("[Drive] codigo: %s em %s\n", userCode, verificationUrl);
  if (showCode) showCode(userCode, verificationUrl);

  String pollBody = "client_id=" + urlEncode(cfg.driveClientId) +
                    "&client_secret=" + urlEncode(cfg.driveClientSecret) +
                    "&device_code=" + urlEncode(deviceCode) +
                    "&grant_type=" + urlEncode("urn:ietf:params:oauth:grant-type:device_code");

  uint32_t deadline = millis() + expiresIn * 1000UL;
  while (millis() < deadline) {
    delay(interval * 1000UL);

    HttpResponse pollResp;
    httpsPostSmall("oauth2.googleapis.com", "/token", "application/x-www-form-urlencoded", "",
                   pollBody, pollResp);

    char refreshToken[256];
    if (jsonGetString(pollResp.body, "refresh_token", refreshToken, sizeof(refreshToken))) {
      settingsStore.saveDriveRefreshToken(refreshToken);
      Serial.println("[Drive] autorizado, refresh token salvo.");
      return true;
    }

    char errorCode[32];
    if (jsonGetString(pollResp.body, "error", errorCode, sizeof(errorCode))) {
      if (strcmp(errorCode, "authorization_pending") == 0) continue;
      if (strcmp(errorCode, "slow_down") == 0) {
        interval += 5;
        continue;
      }
      Serial.printf("[Drive] pareamento cancelado: %s\n", errorCode);
      return false;
    }
  }

  Serial.println("[Drive] codigo expirou sem confirmacao.");
  return false;
}

bool GDriveClient::refreshAccessToken(Settings &cfg, String &outAccessToken) {
  String body = "client_id=" + urlEncode(cfg.driveClientId) +
                "&client_secret=" + urlEncode(cfg.driveClientSecret) +
                "&refresh_token=" + urlEncode(cfg.driveRefreshToken) +
                "&grant_type=refresh_token";
  HttpResponse resp;
  if (!httpsPostSmall("oauth2.googleapis.com", "/token", "application/x-www-form-urlencoded", "",
                      body, resp) || resp.statusCode != 200) {
    Serial.printf("[Drive] falha ao renovar access token (HTTP %d):\n%s\n",
                  resp.statusCode, resp.body.c_str());
    return false;
  }
  char token[256];
  if (!jsonGetString(resp.body, "access_token", token, sizeof(token))) {
    Serial.println("[Drive] resposta de refresh sem access_token:");
    Serial.println(resp.body);
    return false;
  }
  outAccessToken = token;
  return true;
}

bool GDriveClient::ensureFolder(Settings &cfg, const String &accessToken, String &outFolderId) {
  if (cfg.driveFolderId[0] != '\0') {
    outFolderId = cfg.driveFolderId;
    return true;
  }

  String body = "{\"name\":\"Gravador de Ideias\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
  HttpResponse resp;
  if (!httpsPostSmall("www.googleapis.com", "/drive/v3/files", "application/json",
                      "Authorization: Bearer " + accessToken, body, resp) ||
      (resp.statusCode != 200 && resp.statusCode != 201)) {
    Serial.printf("[Drive] falha ao criar pasta (HTTP %d):\n%s\n",
                  resp.statusCode, resp.body.c_str());
    return false;
  }

  char folderId[64];
  if (!jsonGetString(resp.body, "id", folderId, sizeof(folderId))) {
    Serial.println("[Drive] resposta de criar pasta sem id:");
    Serial.println(resp.body);
    return false;
  }
  outFolderId = folderId;
  return true;
}

bool GDriveClient::uploadFileResumable(const String &accessToken, const String &folderId,
                                       const char *localPath, const char *driveName,
                                       const char *mimeType, char *outDriveId, size_t outDriveIdLen,
                                       char *sessionUrlBuf, size_t sessionUrlBufLen,
                                       size_t &confirmedBytes, SyncProgressFn onProgress,
                                       SyncStage stage) {
  File file = LittleFS.open(localPath, FILE_READ);
  if (!file) {
    Serial.printf("[Drive] nao foi possivel abrir %s\n", localPath);
    return false;
  }
  size_t fileSize = file.size();

  // 1. Se ja temos o ID do arquivo gravado, confirmamos sucesso imediatamente
  if (outDriveId[0] != '\0') {
    file.close();
    return true;
  }

  // 2. Se ja existe uma sessao resumable salva, consulta a posicao confirmada
  if (sessionUrlBuf[0] != '\0') {
    String sessionUrl = sessionUrlBuf;
    String host, path;
    uint16_t port = 443;
    if (HttpClient::parseHttpsUrl(sessionUrl, host, port, path)) {
      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(HttpClient::kResponseTimeoutMs);

      if (client.connect(host.c_str(), port)) {
        String req = "PUT " + path + " HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" +
                     "Content-Length: 0\r\n" +
                     "Content-Range: bytes */" + String((unsigned)fileSize) + "\r\n" +
                     "Connection: close\r\n\r\n";
        HttpClient::writeAll(client, req);

        HttpResponse statusResp;
        if (HttpClient::readResponse(client, statusResp, HttpClient::kMaxResponseBody)) {
          if (statusResp.statusCode == 308) {
            int dash = statusResp.range.indexOf('-');
            if (dash >= 0) {
              confirmedBytes = (size_t)strtoull(statusResp.range.substring(dash + 1).c_str(), nullptr, 10) + 1;
              Serial.printf("[Drive] Retomando sessao existente de %s em %u/%u bytes\n",
                            localPath, (unsigned)confirmedBytes, (unsigned)fileSize);
            }
          } else if (statusResp.statusCode == 200 || statusResp.statusCode == 201) {
            jsonGetString(statusResp.body, "id", outDriveId, outDriveIdLen);
            confirmedBytes = fileSize;
            sessionUrlBuf[0] = '\0';
            file.close();
            Serial.printf("[Drive] Arquivo %s ja havia sido concluido no servidor.\n", localPath);
            return true;
          } else {
            // Sessao expirou ou invalida no servidor; recria do zero
            Serial.printf("[Drive] Sessao anterior retornou HTTP %d, recriando do zero...\n", statusResp.statusCode);
            sessionUrlBuf[0] = '\0';
            confirmedBytes = 0;
          }
        } else {
          sessionUrlBuf[0] = '\0';
          confirmedBytes = 0;
        }
      } else {
        sessionUrlBuf[0] = '\0';
        confirmedBytes = 0;
      }
    } else {
      sessionUrlBuf[0] = '\0';
      confirmedBytes = 0;
    }
  }

  // 3. Se nao tem sessao valida, inicia uma nova sessao resumable
  if (sessionUrlBuf[0] == '\0') {
    String metaBody = "{\"name\":\"" + String(driveName) + "\",\"parents\":[\"" + folderId + "\"]}";
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(HttpClient::kResponseTimeoutMs);

    if (!client.connect("www.googleapis.com", 443)) {
      Serial.println("[Drive] Falha ao conectar em www.googleapis.com para iniciar upload resumable.");
      file.close();
      return false;
    }

    String req = "POST /upload/drive/v3/files?uploadType=resumable HTTP/1.1\r\n"
                 "Host: www.googleapis.com\r\n"
                 "Authorization: Bearer " + accessToken + "\r\n"
                 "X-Upload-Content-Type: " + String(mimeType) + "\r\n"
                 "X-Upload-Content-Length: " + String((unsigned)fileSize) + "\r\n"
                 "Content-Type: application/json; charset=UTF-8\r\n"
                 "Content-Length: " + String((unsigned)metaBody.length()) + "\r\n"
                 "Connection: close\r\n\r\n" +
                 metaBody;

    if (!HttpClient::writeAll(client, req)) {
      client.stop();
      file.close();
      return false;
    }

    HttpResponse initResp;
    if (!HttpClient::readResponse(client, initResp, HttpClient::kMaxResponseBody) ||
        (initResp.statusCode != 200 && initResp.statusCode != 201)) {
      Serial.printf("[Drive] Falha ao iniciar upload resumable (HTTP %d):\n%s\n",
                    initResp.statusCode, initResp.body.c_str());
      client.stop();
      file.close();
      return false;
    }

    if (initResp.location.length() == 0) {
      Serial.println("[Drive] Resposta de sessao resumable sem header Location.");
      client.stop();
      file.close();
      return false;
    }

    strncpy(sessionUrlBuf, initResp.location.c_str(), sessionUrlBufLen - 1);
    sessionUrlBuf[sessionUrlBufLen - 1] = '\0';
    confirmedBytes = 0;
  }

  // 4. Transmissao dos blocos (chunks de 256 KiB)
  constexpr size_t kChunkSize = 256 * 1024;
  String sessionUrl = sessionUrlBuf;
  String host, path;
  uint16_t port = 443;
  if (!HttpClient::parseHttpsUrl(sessionUrl, host, port, path)) {
    Serial.println("[Drive] URL de sessao resumable invalida.");
    file.close();
    return false;
  }

  while (confirmedBytes < fileSize) {
    size_t chunkLen = min(kChunkSize, fileSize - confirmedBytes);
    size_t startByte = confirmedBytes;
    size_t endByte = startByte + chunkLen - 1;

    bool chunkOk = false;
    constexpr int kMaxRetries = 4;
    for (int attempt = 0; attempt < kMaxRetries; attempt++) {
      if (onProgress) {
        SyncProgress p;
        p.stage = stage;
        p.fileName = driveName;
        p.bytesUploaded = confirmedBytes;
        p.totalBytes = fileSize;
        p.attempt = attempt + 1;
        p.maxAttempts = kMaxRetries;
        p.message = "enviando...";
        onProgress(p);
      }

      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(HttpClient::kResponseTimeoutMs);

      if (!client.connect(host.c_str(), port)) {
        uint32_t delayMs = HttpClient::retryDelayMs(attempt, 0);
        delay(delayMs);
        continue;
      }

      String reqHeaders = "PUT " + path + " HTTP/1.1\r\n" +
                          "Host: " + host + "\r\n" +
                          "Content-Length: " + String((unsigned)chunkLen) + "\r\n" +
                          "Content-Range: bytes " + String((unsigned)startByte) + "-" +
                          String((unsigned)endByte) + "/" + String((unsigned)fileSize) + "\r\n" +
                          "Connection: close\r\n\r\n";

      if (!HttpClient::writeAll(client, reqHeaders)) {
        client.stop();
        uint32_t delayMs = HttpClient::retryDelayMs(attempt, 0);
        delay(delayMs);
        continue;
      }

      if (!HttpClient::writeFileChunk(client, file, startByte, chunkLen)) {
        client.stop();
        uint32_t delayMs = HttpClient::retryDelayMs(attempt, 0);
        delay(delayMs);
        continue;
      }

      HttpResponse chunkResp;
      if (!HttpClient::readResponse(client, chunkResp, HttpClient::kMaxResponseBody)) {
        client.stop();
        uint32_t delayMs = HttpClient::retryDelayMs(attempt, 0);
        delay(delayMs);
        continue;
      }

      if (chunkResp.statusCode == 308) {
        int dash = chunkResp.range.indexOf('-');
        if (dash >= 0) {
          confirmedBytes = (size_t)strtoull(chunkResp.range.substring(dash + 1).c_str(), nullptr, 10) + 1;
        } else {
          confirmedBytes = endByte + 1;
        }
        chunkOk = true;
        break;
      } else if (chunkResp.statusCode == 200 || chunkResp.statusCode == 201) {
        jsonGetString(chunkResp.body, "id", outDriveId, outDriveIdLen);
        confirmedBytes = fileSize;
        sessionUrlBuf[0] = '\0';

        // Verificacao de checksum MD5 retornado pelo Google Drive
        char remoteMd5[36] = "";
        if (jsonGetString(chunkResp.body, "md5Checksum", remoteMd5, sizeof(remoteMd5))) {
          String localMd5 = calculateFileMd5(localPath);
          if (localMd5.length() > 0 && !localMd5.equalsIgnoreCase(remoteMd5)) {
            Serial.printf("[Drive] ERRO: MD5 divergente para %s (local: %s, drive: %s)!\n",
                          localPath, localMd5.c_str(), remoteMd5);
            file.close();
            return false;
          }
          Serial.printf("[Drive] MD5 verificado com sucesso (%s).\n", remoteMd5);
        }

        chunkOk = true;
        break;
      } else if (HttpClient::isRetryableStatus(chunkResp.statusCode, chunkResp.body)) {
        uint32_t delayMs = HttpClient::retryDelayMs(attempt, chunkResp.retryAfterSec);
        if (delayMs > 60000) {
          file.close();
          return false;
        }
        delay(delayMs);
        continue;
      } else {
        Serial.printf("[Drive] Erro nao repetivel no upload (HTTP %d):\n%s\n",
                      chunkResp.statusCode, chunkResp.body.c_str());
        file.close();
        return false;
      }
    }

    if (!chunkOk) {
      Serial.printf("[Drive] Falha ao enviar bloco de %s apos tentativas.\n", localPath);
      file.close();
      return false;
    }
  }

  file.close();
  return (outDriveId[0] != '\0');
}

bool GDriveClient::getNoteSyncState(const char *wavPath, NoteSyncState &outState) {
  return outState.load(wavPath);
}

bool GDriveClient::uploadNote(SettingsStore &settingsStore, const char *wavPath,
                               const char *txtPath, const char *mdPath,
                               SyncProgressFn onProgress) {
  Settings &cfg = settingsStore.get();
  if (!settingsStore.hasDriveAuth()) {
    Serial.println("[Drive] ainda nao pareado.");
    return false;
  }

  NoteSyncState state;
  state.load(wavPath);

  if (state.fullySynced) {
    Serial.println("[Drive] Nota ja totalmente sincronizada.");
    return true;
  }

  String accessToken;
  if (!refreshAccessToken(cfg, accessToken)) {
    state.lastStatusCode = 401;
    strncpy(state.lastError, "auth_refresh_failed", sizeof(state.lastError) - 1);
    state.save(wavPath);
    return false;
  }

  String folderId;
  if (!ensureFolder(cfg, accessToken, folderId)) {
    state.lastStatusCode = 500;
    strncpy(state.lastError, "ensure_folder_failed", sizeof(state.lastError) - 1);
    state.save(wavPath);
    return false;
  }
  if (folderId != cfg.driveFolderId) {
    settingsStore.saveDriveFolderId(folderId.c_str());
  }

  String wavName = String(wavPath);
  int slash = wavName.lastIndexOf('/');
  if (slash >= 0) wavName = wavName.substring(slash + 1);

  // 1. Upload do WAV (retomavel)
  if (!state.wavUploaded) {
    bool ok = uploadFileResumable(accessToken, folderId, wavPath, wavName.c_str(),
                                  "audio/wav", state.wavDriveId, sizeof(state.wavDriveId),
                                  state.wavSessionUrl, sizeof(state.wavSessionUrl),
                                  state.wavBytesUploaded, onProgress, SyncStage::UploadingWav);
    state.lastAttemptTime = millis();
    if (!ok) {
      strncpy(state.lastError, "wav_upload_failed", sizeof(state.lastError) - 1);
      state.save(wavPath);
      return false;
    }
    state.wavUploaded = true;
    state.save(wavPath);
  }

  // 2. Upload do TXT (se existir)
  if (txtPath && LittleFS.exists(txtPath) && !state.txtUploaded) {
    String txtName = String(txtPath);
    int s2 = txtName.lastIndexOf('/');
    if (s2 >= 0) txtName = txtName.substring(s2 + 1);

    char txtSession[256] = "";
    size_t txtBytes = 0;
    bool ok = uploadFileResumable(accessToken, folderId, txtPath, txtName.c_str(),
                                  "text/plain; charset=utf-8", state.txtDriveId,
                                  sizeof(state.txtDriveId), txtSession, sizeof(txtSession),
                                  txtBytes, onProgress, SyncStage::UploadingTxt);
    if (!ok) {
      strncpy(state.lastError, "txt_upload_failed", sizeof(state.lastError) - 1);
      state.save(wavPath);
      return false;
    }
    state.txtUploaded = true;
    state.save(wavPath);
  } else if (!txtPath || !LittleFS.exists(txtPath)) {
    // Se nao ha txt configurado/existente, considera concluida essa etapa
    state.txtUploaded = true;
  }

  // 3. Upload do MD (se existir)
  if (mdPath && LittleFS.exists(mdPath) && !state.mdUploaded) {
    String mdName = String(mdPath);
    int s3 = mdName.lastIndexOf('/');
    if (s3 >= 0) mdName = mdName.substring(s3 + 1);

    char mdSession[256] = "";
    size_t mdBytes = 0;
    bool ok = uploadFileResumable(accessToken, folderId, mdPath, mdName.c_str(),
                                  "text/markdown; charset=utf-8", state.mdDriveId,
                                  sizeof(state.mdDriveId), mdSession, sizeof(mdSession),
                                  mdBytes, onProgress, SyncStage::UploadingMd);
    if (!ok) {
      strncpy(state.lastError, "md_upload_failed", sizeof(state.lastError) - 1);
      state.save(wavPath);
      return false;
    }
    state.mdUploaded = true;
    state.save(wavPath);
  } else if (!mdPath || !LittleFS.exists(mdPath)) {
    // Se nao ha md configurado/existente, considera concluida essa etapa
    state.mdUploaded = true;
  }

  // Se chegou aqui, todos os arquivos foram confirmados no Drive
  state.fullySynced = (state.wavUploaded && state.txtUploaded && state.mdUploaded);
  state.lastStatusCode = 200;
  state.lastError[0] = '\0';
  state.save(wavPath);

  if (onProgress) {
    SyncProgress p;
    p.stage = SyncStage::Done;
    p.fileName = wavName.c_str();
    p.bytesUploaded = state.wavBytesUploaded;
    p.totalBytes = state.wavBytesUploaded;
    p.message = "concluido";
    onProgress(p);
  }

  return state.fullySynced;
}
