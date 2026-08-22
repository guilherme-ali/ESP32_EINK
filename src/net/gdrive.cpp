#include "gdrive.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>

namespace {

bool extractJsonString(const String &body, const char *key, char *out, size_t outLen) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return false;
  int colon = body.indexOf(':', k);
  if (colon < 0) return false;
  int start = body.indexOf('"', colon + 1);
  if (start < 0) return false;
  start++;

  size_t o = 0;
  int i = start;
  while (i < (int)body.length() && o < outLen - 1) {
    char c = body[i];
    if (c == '"') break;
    if (c == '\\' && i + 1 < (int)body.length()) {
      out[o++] = body[i + 1];
      i += 2;
      continue;
    }
    out[o++] = c;
    i++;
  }
  out[o] = '\0';
  return o > 0;
}

long extractJsonInt(const String &body, const char *key, long fallback) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return fallback;
  int colon = body.indexOf(':', k);
  if (colon < 0) return fallback;
  return body.substring(colon + 1).toInt();
}

// POST simples com corpo ja pronto (form-urlencoded ou JSON pequeno) -
// usado para os endpoints OAuth e para criar a pasta no Drive. Corpos
// grandes (upload do WAV) tem uma rotina propria em uploadFile().
bool httpsPostSmall(const String &host, const String &path, const String &contentType,
                     const String &authHeader, const String &body, String &responseBody) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  if (!client.connect(host.c_str(), 443)) {
    Serial.printf("[Drive] falha ao conectar em %s\n", host.c_str());
    return false;
  }

  client.printf("POST %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", host.c_str());
  if (authHeader.length() > 0) client.print(authHeader + "\r\n");
  client.printf("Content-Type: %s\r\n", contentType.c_str());
  client.printf("Content-Length: %u\r\n", (unsigned)body.length());
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 20000) delay(20);
  delay(50);

  String statusLine = client.readStringUntil('\n');
  for (int attempt = 0; statusLine.length() == 0 && attempt < 5 && client.connected(); attempt++) {
    delay(50);
    statusLine = client.readStringUntil('\n');
  }
  bool ok = statusLine.indexOf(" 200 ") >= 0;

  bool chunked = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.indexOf("Transfer-Encoding") >= 0 && line.indexOf("chunked") >= 0) chunked = true;
    if (line == "\r" || line.length() == 0) break;
  }

  responseBody = "";
  responseBody.reserve(1024);
  if (chunked) {
    while (client.connected() || client.available()) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      long read = 0;
      while (read < chunkSize) {
        if (client.available()) {
          responseBody += (char)client.read();
          read++;
        } else if (!client.connected()) {
          break;
        }
      }
      client.readStringUntil('\n');
      if (responseBody.length() > 4096) break;
    }
  } else {
    while (client.available() || client.connected()) {
      while (client.available()) {
        responseBody += (char)client.read();
        if (responseBody.length() > 4096) break;
      }
      if (!client.connected() && !client.available()) break;
    }
  }
  client.stop();
  return ok;
}

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

} // namespace

bool GDriveClient::pairDevice(SettingsStore &settingsStore, ShowCodeFn showCode) {
  Settings &cfg = settingsStore.get();
  if (cfg.driveClientId[0] == '\0') {
    Serial.println("[Drive] Client ID nao configurado.");
    return false;
  }

  String reqBody = "client_id=" + urlEncode(cfg.driveClientId) +
                    "&scope=" + urlEncode("https://www.googleapis.com/auth/drive.file");
  String resp;
  if (!httpsPostSmall("oauth2.googleapis.com", "/device/code",
                       "application/x-www-form-urlencoded", "", reqBody, resp)) {
    Serial.println("[Drive] falha ao pedir device code:");
    Serial.println(resp);
    return false;
  }

  char deviceCode[128], userCode[32], verificationUrl[64];
  if (!extractJsonString(resp, "device_code", deviceCode, sizeof(deviceCode)) ||
      !extractJsonString(resp, "user_code", userCode, sizeof(userCode)) ||
      !extractJsonString(resp, "verification_url", verificationUrl, sizeof(verificationUrl))) {
    Serial.println("[Drive] resposta de device_code incompleta:");
    Serial.println(resp);
    return false;
  }
  long interval = extractJsonInt(resp, "interval", 5);
  long expiresIn = extractJsonInt(resp, "expires_in", 1800);

  Serial.printf("[Drive] codigo: %s em %s\n", userCode, verificationUrl);
  if (showCode) showCode(userCode, verificationUrl);

  String pollBody = "client_id=" + urlEncode(cfg.driveClientId) +
                     "&client_secret=" + urlEncode(cfg.driveClientSecret) +
                     "&device_code=" + urlEncode(deviceCode) +
                     "&grant_type=" + urlEncode("urn:ietf:params:oauth:grant-type:device_code");

  uint32_t deadline = millis() + expiresIn * 1000UL;
  while (millis() < deadline) {
    delay(interval * 1000UL);

    String pollResp;
    httpsPostSmall("oauth2.googleapis.com", "/token", "application/x-www-form-urlencoded", "",
                    pollBody, pollResp);

    char refreshToken[256];
    if (extractJsonString(pollResp, "refresh_token", refreshToken, sizeof(refreshToken))) {
      settingsStore.saveDriveRefreshToken(refreshToken);
      Serial.println("[Drive] autorizado, refresh token salvo.");
      return true;
    }

    char errorCode[32];
    if (extractJsonString(pollResp, "error", errorCode, sizeof(errorCode))) {
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
  String resp;
  if (!httpsPostSmall("oauth2.googleapis.com", "/token", "application/x-www-form-urlencoded", "",
                       body, resp)) {
    Serial.println("[Drive] falha ao renovar access token:");
    Serial.println(resp);
    return false;
  }
  char token[256];
  if (!extractJsonString(resp, "access_token", token, sizeof(token))) {
    Serial.println("[Drive] resposta de refresh sem access_token:");
    Serial.println(resp);
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
  String resp;
  if (!httpsPostSmall("www.googleapis.com", "/drive/v3/files", "application/json",
                       "Authorization: Bearer " + accessToken, body, resp)) {
    Serial.println("[Drive] falha ao criar pasta:");
    Serial.println(resp);
    return false;
  }

  char folderId[64];
  if (!extractJsonString(resp, "id", folderId, sizeof(folderId))) {
    Serial.println("[Drive] resposta de criar pasta sem id:");
    Serial.println(resp);
    return false;
  }
  outFolderId = folderId;
  return true;
}

bool GDriveClient::uploadFile(const String &accessToken, const String &folderId,
                               const char *localPath, const char *driveName, const char *mimeType) {
  File file = LittleFS.open(localPath, FILE_READ);
  if (!file) {
    Serial.printf("[Drive] nao abriu %s\n", localPath);
    return false;
  }
  size_t fileSize = file.size();

  const String boundary = "----ideiarecdrive39f2";
  String metadata = String("{\"name\":\"") + driveName + "\",\"parents\":[\"" + folderId + "\"]}";
  String partMeta = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                     metadata + "\r\n";
  String partFileHeader = "--" + boundary + "\r\nContent-Type: " + mimeType + "\r\n\r\n";
  String partFooter = "\r\n--" + boundary + "--\r\n";
  size_t contentLength = partMeta.length() + partFileHeader.length() + fileSize + partFooter.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  if (!client.connect("www.googleapis.com", 443)) {
    Serial.println("[Drive] falha ao conectar para upload.");
    file.close();
    return false;
  }

  client.print("POST /upload/drive/v3/files?uploadType=multipart HTTP/1.1\r\n");
  client.print("Host: www.googleapis.com\r\n");
  client.print("Authorization: Bearer " + accessToken + "\r\n");
  client.printf("Content-Type: multipart/related; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.print("Connection: close\r\n\r\n");

  client.print(partMeta);
  client.print(partFileHeader);

  uint8_t buf[1024];
  while (file.available()) {
    size_t n = file.read(buf, sizeof(buf));
    client.write(buf, n);
  }
  file.close();
  client.print(partFooter);

  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 20000) delay(20);
  delay(50);
  String statusLine = client.readStringUntil('\n');
  bool ok = statusLine.indexOf(" 200 ") >= 0;
  client.stop();

  if (!ok) Serial.printf("[Drive] upload de %s falhou: %s\n", localPath, statusLine.c_str());
  return ok;
}

bool GDriveClient::uploadNote(SettingsStore &settingsStore, const char *wavPath, const char *txtPath) {
  Settings &cfg = settingsStore.get();
  if (!settingsStore.hasDriveAuth()) {
    Serial.println("[Drive] ainda nao pareado.");
    return false;
  }

  String accessToken;
  if (!refreshAccessToken(cfg, accessToken)) return false;

  String folderId;
  if (!ensureFolder(cfg, accessToken, folderId)) return false;
  if (folderId != cfg.driveFolderId) settingsStore.saveDriveFolderId(folderId.c_str());

  String wavName = String(wavPath);
  int slash = wavName.lastIndexOf('/');
  if (slash >= 0) wavName = wavName.substring(slash + 1);

  bool ok = uploadFile(accessToken, folderId, wavPath, wavName.c_str(), "audio/wav");

  if (ok && txtPath && LittleFS.exists(txtPath)) {
    String txtName = String(txtPath);
    int s2 = txtName.lastIndexOf('/');
    if (s2 >= 0) txtName = txtName.substring(s2 + 1);
    uploadFile(accessToken, folderId, txtPath, txtName.c_str(), "text/plain; charset=utf-8");
  }

  return ok;
}
