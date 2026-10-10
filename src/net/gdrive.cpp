#include "gdrive.h"
#include "http_client.h"
#include "../storage/note_files.h"
#include "../sync/telemetry.h"
#include <MD5Builder.h>
#include <cJSON.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <utility>

namespace {

constexpr size_t kMaxStateBytes = 32768;
constexpr size_t kChunkBytes = 1024 * 1024;
constexpr size_t kMultipartBytes = 5 * 1024 * 1024;
constexpr uint32_t kFolderCacheMs = 30000;
constexpr int kMaxAttempts = 4;
const char *kFolderName = "Gravador de Ideias";
const char *kFolderMime = "application/vnd.google-apps.folder";
const char *kFileFields = "id,name,size,md5Checksum,trashed,parents,mimeType";
const char *kFolderFields = "id,name,mimeType,trashed";
const char *kAssetPrefix[] = {"wav", "txt", "md"};

// RAII: nenhuma resposta OAuth e registrada; os corpos ficam apenas em RAM.
struct Json {
  cJSON *p;
  explicit Json(cJSON *value = nullptr) : p(value) {}
  ~Json() { cJSON_Delete(p); }
  Json(const Json &) = delete;
  Json &operator=(const Json &) = delete;
};

cJSON *parseJson(const String &text) {
  // cJSON aceita um prefixo por padrao. Rejeitar bytes/escapes NUL evita
  // truncamento silencioso de IDs, URLs ou tokens apos a decodificacao.
  if (strlen(text.c_str()) != text.length() || text.indexOf("\\u0000") >= 0) return nullptr;
  // Limitar profundidade antes de entrar no parser recursivo de cJSON.
  // Metadados Drive/OAuth e o estado usam poucas camadas de objetos/arrays.
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 16) return nullptr; }
    else if (c == '}' || c == ']') { if (!depth) return nullptr; --depth; }
  }
  cJSON *root = cJSON_ParseWithOpts(text.c_str(), nullptr, true);
  if (root && !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return nullptr;
  }
  return root;
}

cJSON *item(const cJSON *root, const char *name) {
  return cJSON_GetObjectItemCaseSensitive(root, name);
}

bool plainString(const char *s) {
  if (!s) return false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
    if (*p < 32 || *p == 127) return false;
  }
  return true;
}

bool getString(const cJSON *root, const char *key, String &out, size_t maxLength,
               bool allowEmpty = false) {
  cJSON *v = item(root, key);
  if (!cJSON_IsString(v) || !plainString(v->valuestring)) return false;
  size_t n = strlen(v->valuestring);
  if ((!allowEmpty && n == 0) || n > maxLength) return false;
  out = v->valuestring;
  return out.length() == n;
}

bool copyString(char *out, size_t capacity, const String &s) {
  if (s.length() >= capacity) return false;
  memcpy(out, s.c_str(), s.length() + 1);
  return true;
}

void diagnostic(char *out, size_t capacity, const String &s) {
  // Somente diagnosticos de UI sao limitados, nunca tokens/IDs/sessoes.
  size_t n = s.length() < capacity - 1 ? s.length() : capacity - 1;
  memcpy(out, s.c_str(), n);
  out[n] = '\0';
}

bool isId(const String &s) {
  if (s.isEmpty() || s.length() >= 128) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

bool isMd5(const String &s) {
  if (s.length() != 32) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool getUnsigned(const cJSON *root, const char *key, uint64_t maxValue, uint64_t &out) {
  cJSON *v = item(root, key);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
      v->valuedouble > static_cast<double>(maxValue) ||
      floor(v->valuedouble) != v->valuedouble) return false;
  out = static_cast<uint64_t>(v->valuedouble);
  return true;
}

uint32_t tokenLifetimeMs(const cJSON *root) {
  uint64_t expires = 0;
  if (!getUnsigned(root, "expires_in", INT32_MAX / 1000, expires) || !expires) return 0;
  uint32_t ttl = static_cast<uint32_t>(expires * 1000);
  return ttl - (ttl > 120000 ? 60000 : ttl / 2);
}

bool decimal(const String &s, size_t &out) {
  if (s.isEmpty()) return false;
  size_t n = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    size_t d = static_cast<size_t>(s[i] - '0');
    if (n > (SIZE_MAX - d) / 10) return false;
    n = n * 10 + d;
  }
  out = n;
  return true;
}

String urlEncode(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

String queryLiteral(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] == '\\' || s[i] == '\'') out += '\\';
    out += s[i];
  }
  return "'" + out + "'";
}

String siblingPath(const char *wavPath, const char *extension) {
  if (!wavPath || !*wavPath) return "";
  String p(wavPath);
  int dot = p.lastIndexOf('.');
  if (dot > p.lastIndexOf('/')) p = p.substring(0, dot);
  return p + extension;
}

String baseName(const char *path) {
  String name(path);
  return name.substring(name.lastIndexOf('/') + 1);
}

bool sessionParts(const String &url, String &host, uint16_t &port, String &path) {
  if (url.isEmpty() || !plainString(url.c_str()) || url.length() > 4096 ||
      !HttpClient::parseHttpsUrl(url, host, port, path)) return false;
  // Nao encaminhar Bearer a um host arbitrario de um estado local corrompido.
  return port == 443 && (host == "googleapis.com" || host.endsWith(".googleapis.com")) &&
         path.startsWith("/") && plainString(path.c_str());
}

// Guarda de mudanca local, independente do MD5 remoto. Nunca reutiliza hashes
// do .sync como prova local. O job controla a fotografia; arquivos devem ficar
// imoveis durante o job, mas as guardas ainda detectam alteracoes de conteudo.
struct ContentGuard {
  uint64_t first = 14695981039346656037ULL;
  uint64_t second = 0x9e3779b97f4a7c15ULL;
  void add(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      first = (first ^ bytes[i]) * 1099511628211ULL;
      second ^= bytes[i] + 0x9e3779b97f4a7c15ULL + (second << 6) + (second >> 2);
    }
  }
  bool equals(const ContentGuard &other) const {
    return first == other.first && second == other.second;
  }
};

bool fileSnapshot(const char *path, bool text, size_t &size, String &hash,
                  ContentGuard *guard = nullptr) {
  if (!path || !*path || (text ? !NoteFiles::validText(path) : !NoteFiles::validWav(path))) return false;
  File f = NoteFiles::openRead(path);
  if (!f || f.isDirectory() || f.size() == 0) return false;
  size = f.size();
  MD5Builder md5;
  md5.begin();
  uint8_t buffer[1024];
  size_t remaining = size;
  while (remaining) {
    size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t n = f.read(buffer, wanted);
    if (n == 0 || n > wanted) return false;
    md5.add(buffer, static_cast<uint16_t>(n));
    if (guard) guard->add(buffer, n);
    remaining -= n;
    yield();
  }
  if (f.size() != size) return false;
  md5.calculate();
  hash = md5.toString();
  return isMd5(hash);
}

struct Asset {
  bool &uploaded;
  char *id;
  bool &reserved;
  String &session;
  size_t &offset;
  char *hash;
  size_t &size;
};

Asset asset(NoteSyncState &s, int index) {
  if (index == 0) return {s.wavUploaded, s.wavDriveId, s.wavIdReserved, s.wavSessionUrl,
                         s.wavBytesUploaded, s.wavMd5, s.wavSize};
  if (index == 1) return {s.txtUploaded, s.txtDriveId, s.txtIdReserved, s.txtSessionUrl,
                         s.txtBytesUploaded, s.txtMd5, s.txtSize};
  return {s.mdUploaded, s.mdDriveId, s.mdIdReserved, s.mdSessionUrl, s.mdBytesUploaded, s.mdMd5, s.mdSize};
}

struct ConstAsset {
  bool uploaded;
  const char *id;
  bool reserved;
  const String &session;
  size_t offset;
  const char *hash;
  size_t size;
};

ConstAsset asset(const NoteSyncState &s, int index) {
  if (index == 0) return {s.wavUploaded, s.wavDriveId, s.wavIdReserved, s.wavSessionUrl,
                         s.wavBytesUploaded, s.wavMd5, s.wavSize};
  if (index == 1) return {s.txtUploaded, s.txtDriveId, s.txtIdReserved, s.txtSessionUrl,
                         s.txtBytesUploaded, s.txtMd5, s.txtSize};
  return {s.mdUploaded, s.mdDriveId, s.mdIdReserved, s.mdSessionUrl, s.mdBytesUploaded, s.mdMd5, s.mdSize};
}

bool boolField(const cJSON *root, const char *name, bool &out) {
  cJSON *v = item(root, name);
  if (!cJSON_IsBool(v)) return false;
  out = cJSON_IsTrue(v);
  return true;
}

bool decodeState(const String &json, NoteSyncState &out) {
  Json root(parseJson(json));
  if (!root.p) return false;
  uint64_t version = 0;
  if (!getUnsigned(root.p, "version", NoteSyncState::kCurrentVersion, version) || version < 1) return false;
  NoteSyncState candidate;
  candidate.version = static_cast<uint8_t>(version);
  if (version >= 3) {
    uint64_t generation;
    if (!getUnsigned(root.p, "authGeneration", UINT32_MAX, generation)) return false;
    candidate.authGeneration = static_cast<uint32_t>(generation);
  }
  if (version == 1) {
    // Flags v1/.snc nao comprovavam MD5 nem distinguiam ID reservado.
    // IDs v1 podiam ter sido truncados a 47 bytes. Validar a estrutura,
    // mas reconciliar por nome+pasta+hash antes de reservar qualquer novo ID.
    for (int i = 0; i < 3; ++i) {
      String key = String(kAssetPrefix[i]) + "DriveId";
      if (item(root.p, key.c_str())) {
        String id;
        if (!getString(root.p, key.c_str(), id, 127, true) ||
            (!id.isEmpty() && !isId(id))) return false;
      }
    }
    out = std::move(candidate);
    return true;
  }
  if (!boolField(root.p, "fullySynced", candidate.fullySynced)) return false;
  String folder, pendingFolder;
  if (!getString(root.p, "folderId", folder, 127, true) ||
      (!folder.isEmpty() && !isId(folder)) || !copyString(candidate.folderId, 128, folder) ||
      !getString(root.p, "pendingFolderId", pendingFolder, 127, true) ||
      (!pendingFolder.isEmpty() && !isId(pendingFolder)) ||
      !copyString(candidate.pendingFolderId, 128, pendingFolder)) return false;
  for (int i = 0; i < 3; ++i) {
    Asset a = asset(candidate, i);
    String prefix(kAssetPrefix[i]), id, hash, session;
    uint64_t size = 0, offset = 0;
    if (!boolField(root.p, (prefix + "Uploaded").c_str(), a.uploaded) ||
        !boolField(root.p, (prefix + "IdReserved").c_str(), a.reserved) ||
        !getString(root.p, (prefix + "DriveId").c_str(), id, 127, true) ||
        (!id.isEmpty() && !isId(id)) || !copyString(a.id, 128, id) ||
        !getString(root.p, (prefix + "Md5").c_str(), hash, 32, true) ||
        (!hash.isEmpty() && !isMd5(hash)) || !copyString(a.hash, 33, hash) ||
        !getString(root.p, (prefix + "SessionUrl").c_str(), session, 4096, true) ||
        !getUnsigned(root.p, (prefix + "Size").c_str(), SIZE_MAX, size) ||
        !getUnsigned(root.p, (prefix + "BytesUploaded").c_str(), size, offset)) return false;
    a.size = static_cast<size_t>(size);
    a.offset = static_cast<size_t>(offset);
    if (a.reserved && id.isEmpty()) return false;
    if (!session.isEmpty()) {
      String host, path;
      uint16_t port;
      if (!sessionParts(session, host, port, path) || id.isEmpty() || hash.isEmpty() || !size) return false;
    }
    a.session = std::move(session);
    if (a.uploaded && (a.reserved || id.isEmpty() || hash.isEmpty() || !size || a.offset != a.size ||
                       !a.session.isEmpty() || folder.isEmpty())) return false;
  }
  cJSON *status = item(root.p, "lastStatusCode");
  uint64_t when;
  String error;
  if (!cJSON_IsNumber(status) || status->valuedouble < 0 || status->valuedouble > 599 ||
      floor(status->valuedouble) != status->valuedouble ||
      !getUnsigned(root.p, "lastAttemptTime", UINT32_MAX, when) ||
      !getString(root.p, "lastError", error, sizeof(candidate.lastError) - 1, true)) return false;
  candidate.lastStatusCode = status->valueint;
  candidate.lastAttemptTime = static_cast<uint32_t>(when);
  copyString(candidate.lastError, sizeof(candidate.lastError), error);
  if (candidate.fullySynced && (!candidate.wavUploaded || folder.isEmpty() || !pendingFolder.isEmpty())) return false;
  out = std::move(candidate);
  return true;
}

bool readState(const String &path, NoteSyncState &out, String *raw = nullptr) {
  File f = NoteFiles::fs().open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory() || f.size() == 0 || f.size() > kMaxStateBytes) return false;
  size_t size = f.size();
  String json;
  if (!NoteFiles::readBounded(f, json, kMaxStateBytes) || json.length() != size || !decodeState(json, out)) return false;
  if (raw) *raw = json;
  return true;
}

String render(cJSON *root) {
  if (!root) return "";
  char *text = cJSON_PrintUnformatted(root);
  if (!text) return "";
  String out(text);
  cJSON_free(text);
  return out;
}

bool accepted(int code) { return code == 200 || code == 201; }

String httpProblem(const char *operation, const HttpResponse &resp, bool transportOk) {
  String message(operation);
  if (!transportOk) return message + ": conexao interrompida ou resposta HTTP incompleta";
  switch (resp.statusCode) {
    case 401: return message + ": autorizacao expirada; pareie novamente se persistir";
    case 403: return message + ": sem permissao ou limite do Google Drive atingido";
    case 404: return message + ": recurso nao encontrado";
    case 409: return message + ": conflito; arquivo remoto precisa ser verificado";
    case 429: return message + ": limite de requisicoes; tente mais tarde";
    default: return message + ": HTTP " + String(resp.statusCode);
  }
}

String oauthProblem(const HttpResponse &resp, bool transportOk) {
  if (!transportOk) return "OAuth: conexao interrompida ou resposta incompleta";
  Json root(parseJson(resp.body));
  String error;
  if (root.p && getString(root.p, "error", error, 128)) {
    if (error == "access_denied") return "Pareamento recusado pelo usuario";
    if (error == "expired_token") return "Codigo de pareamento expirado";
    if (error == "invalid_grant") return "Autorizacao revogada ou expirada; pareie novamente";
    if (error == "invalid_client") return "Client ID/secret OAuth invalido ou tipo de cliente incorreto";
    if (error == "admin_policy_enforced" || error == "org_internal") return "Pareamento bloqueado pela organizacao Google";
  }
  return "Falha OAuth (HTTP " + String(resp.statusCode) + ")";
}

// Todas as requisicoes usam os timeouts existentes; corpos grandes sao
// transmitidos por streaming. Status real e preservado mesmo em read parcial.
bool transportOn(HttpClient::Connection &connection, bool keepAlive,
                const String &host, uint16_t port, const String &path, const char *method,
                const String &token, const String &extraHeaders, const String &body,
                HttpResponse &response, File *file, size_t offset, size_t length,
                const String &suffix, HttpClient::WriteProgressFn progress, void *context) {
  response = HttpResponse();
  WiFiClientSecure &client = connection.client;
  if (!connection.reusable || connection.host != host || connection.port != port ||
      !client.connected() || client.available()) {
    connection.stop();
    client.setInsecure(); // Mantem a politica TLS atual do projeto.
    client.setHandshakeTimeout((HttpClient::kConnectTimeoutMs + 999) / 1000);
    SyncTelemetry::tlsHandshake(host.c_str());
    if (!client.connect(host.c_str(), port, HttpClient::kConnectTimeoutMs)) {
      connection.stop(); return false;
    }
    connection.host = host;
    connection.port = port;
  }
  connection.reusable = false;
  // WiFiClientSecure::setTimeout usa SEGUNDOS no Arduino-ESP32, enquanto
  // connect(..., timeout) e os helpers HTTP recebem milissegundos.
  client.setTimeout((HttpClient::kResponseTimeoutMs + 999) / 1000);
  String headers = String(method) + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
  if (!token.isEmpty()) headers += "Authorization: Bearer " + token + "\r\n";
  headers += extraHeaders;
  size_t contentLength = body.length();
  if (file) {
    if (length > SIZE_MAX - contentLength || suffix.length() > SIZE_MAX - contentLength - length) {
      connection.stop(); return false;
    }
    contentLength += length + suffix.length();
  }
  headers += "Content-Length: " + String(static_cast<unsigned long>(contentLength)) +
             (keepAlive ? "\r\nConnection: keep-alive\r\n\r\n" : "\r\nConnection: close\r\n\r\n");
  SyncTelemetry::httpRequest(host.c_str());
  bool ok = HttpClient::writeAll(client, headers);
  if (ok && !body.isEmpty()) ok = HttpClient::writeAll(client,
      reinterpret_cast<const uint8_t *>(body.c_str()), body.length(),
      HttpClient::kResponseTimeoutMs, nullptr, nullptr, true);
  if (ok && file) ok = HttpClient::writeFileChunk(client, *file, offset, length,
      HttpClient::kResponseTimeoutMs, progress, context);
  if (ok && file && !suffix.isEmpty()) ok = HttpClient::writeAll(client,
      reinterpret_cast<const uint8_t *>(suffix.c_str()), suffix.length(),
      HttpClient::kResponseTimeoutMs, nullptr, nullptr, true);
  // Mesmo apos write parcial, preservar 401/403/429 antecipados do servidor.
  bool read = HttpClient::readResponse(client, response, HttpClient::kMaxResponseBody);
  bool complete = read && response.headersComplete && response.bodyComplete && !response.bodyTruncated;
  if (ok && complete && !response.connectionClose && keepAlive) connection.reusable = true;
  else connection.stop();
  // Uma resposta de sucesso nunca comprova um request que nao terminou.
  return complete && (ok || response.statusCode >= 400);
}

bool transport(const String &host, uint16_t port, const String &path, const char *method,
                const String &token, const String &extraHeaders, const String &body,
                HttpResponse &response, File *file = nullptr, size_t offset = 0, size_t length = 0,
                HttpClient::Connection *persistent = nullptr, const String &suffix = "",
                HttpClient::WriteProgressFn progress = nullptr, void *context = nullptr) {
  if (persistent) return transportOn(*persistent, true, host, port, path, method,
      token, extraHeaders, body, response, file, offset, length, suffix, progress, context);
  // OAuth nunca deixa sockets/contextos em cache.
  HttpClient::Connection temporary;
  return transportOn(temporary, false, host, port, path, method,
      token, extraHeaders, body, response, file, offset, length, suffix, progress, context);
}

bool oauthPost(const char *path, const String &body, HttpResponse &response) {
  return transport("oauth2.googleapis.com", 443, path, "POST", "",
                   "Content-Type: application/x-www-form-urlencoded\r\n", body, response);
}

bool rangeOffset(const String &range, size_t total, size_t previous, size_t upperBound, size_t &next) {
  String r(range);
  r.trim();
  next = 0; // 308 SEM Range significa zero, nunca o fim do bloco enviado.
  if (!r.isEmpty()) {
    size_t last;
    if (!r.startsWith("bytes=0-") || !decimal(r.substring(8), last) || last >= total) return false;
    next = last + 1;
  }
  return next >= previous && next <= total && next <= upperBound;
}

bool hasParent(const cJSON *root, const String &folder) {
  cJSON *parents = item(root, "parents");
  if (!cJSON_IsArray(parents)) return false;
  cJSON *entry;
  cJSON_ArrayForEach(entry, parents) {
    if (cJSON_IsString(entry) && entry->valuestring && folder == entry->valuestring) return true;
  }
  return false;
}

} // namespace

String NoteSyncState::statePathFor(const char *wavPath) { return siblingPath(wavPath, ".sync"); }
String NoteSyncState::legacySncPathFor(const char *wavPath) { return siblingPath(wavPath, ".snc"); }

bool NoteSyncState::load(const char *wavPath) {
  *this = NoteSyncState();
  String path = statePathFor(wavPath);
  if (path.isEmpty()) {
    diagnostic(lastError, sizeof(lastError), "Caminho de nota invalido");
    return false;
  }
  fs::FS &fs = NoteFiles::fs();
  String tmp = path + ".tmp";
  NoteSyncState candidate;
  // Um .tmp completo e a transicao mais recente, inclusive se o definitivo
  // anterior ainda existe. Nunca usa JSON parcial como prova de conclusao.
  if (fs.exists(tmp.c_str()) && readState(tmp, candidate)) {
    if (!fs.rename(tmp.c_str(), path.c_str())) {
      diagnostic(lastError, sizeof(lastError), "Falha ao recuperar estado temporario");
      return false;
    }
    *this = std::move(candidate);
  } else if (fs.exists(path.c_str())) {
    if (!readState(path, candidate)) {
      diagnostic(lastError, sizeof(lastError), "Estado .sync corrompido ou versao nao suportada");
      return false; // Nao sobrescrever um estado desconhecido e duplicar uploads.
    }
    *this = std::move(candidate);
  } else if (fs.exists(legacySncPathFor(wavPath).c_str())) {
    version = 1; // Marcador e somente uma pista para reconciliacao por nome+MD5.
  } else {
    if (fs.exists(tmp.c_str())) {
      diagnostic(lastError, sizeof(lastError), "Estado temporario corrompido; recuperacao necessaria");
    }
    return false;
  }
  if (version != kCurrentVersion) {
    version = kCurrentVersion;
    if (!save(wavPath)) {
      diagnostic(lastError, sizeof(lastError), "Falha ao persistir migracao do estado de sincronizacao");
      return false;
    }
  }
  return true;
}

bool NoteSyncState::save(const char *wavPath) const {
  if (version != kCurrentVersion) return false;
  String path = statePathFor(wavPath);
  if (path.isEmpty()) return false;
  Json root(cJSON_CreateObject());
  if (!root.p) return false;
  bool ok = cJSON_AddNumberToObject(root.p, "version", version) &&
            cJSON_AddNumberToObject(root.p, "authGeneration", authGeneration) &&
            cJSON_AddBoolToObject(root.p, "fullySynced", fullySynced) &&
            cJSON_AddStringToObject(root.p, "folderId", folderId) &&
            cJSON_AddStringToObject(root.p, "pendingFolderId", pendingFolderId) &&
            cJSON_AddNumberToObject(root.p, "lastStatusCode", lastStatusCode) &&
            cJSON_AddStringToObject(root.p, "lastError", lastError) &&
            cJSON_AddNumberToObject(root.p, "lastAttemptTime", lastAttemptTime);
  for (int i = 0; ok && i < 3; ++i) {
    ConstAsset a = asset(*this, i);
    String prefix(kAssetPrefix[i]);
    ok = cJSON_AddBoolToObject(root.p, (prefix + "Uploaded").c_str(), a.uploaded) &&
         cJSON_AddBoolToObject(root.p, (prefix + "IdReserved").c_str(), a.reserved) &&
         cJSON_AddStringToObject(root.p, (prefix + "DriveId").c_str(), a.id) &&
         cJSON_AddStringToObject(root.p, (prefix + "Md5").c_str(), a.hash) &&
         cJSON_AddStringToObject(root.p, (prefix + "SessionUrl").c_str(), a.session.c_str()) &&
         cJSON_AddNumberToObject(root.p, (prefix + "Size").c_str(), static_cast<double>(a.size)) &&
         cJSON_AddNumberToObject(root.p, (prefix + "BytesUploaded").c_str(), static_cast<double>(a.offset));
  }
  if (!ok) return false;
  String json = render(root.p);
  NoteSyncState decoded;
  if (json.isEmpty() || json.length() > kMaxStateBytes || !decodeState(json, decoded)) return false;
  // writeAtomic e um helper de bytes. Aqui precisamos validar tambem o schema
  // e o conteudo relido ANTES do commit, e recuperar .sync.tmp no boot.
  // LittleFS suporta replacement no rename; nunca remover o definitivo antes.
  if (NoteFiles::freeBytes() < static_cast<uint64_t>(json.length()) + 4096) return false;
  fs::FS &fs = NoteFiles::fs();
  String tmp = path + ".tmp";
  File f = fs.open(tmp.c_str(), FILE_WRITE);
  if (!f) return false;
  size_t written = f.write(reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
  f.flush();
  bool sizeOk = f.size() == json.length();
  f.close();
  if (written != json.length() || !sizeOk) return false;
  String reread;
  if (!readState(tmp, decoded, &reread) || reread != json) return false;
  return fs.rename(tmp.c_str(), path.c_str());
}

bool GDriveClient::fail(const String &message, int statusCode) {
  SyncTelemetry::phase(SyncTelemetry::Phase::Error, "falha Drive");
  driveConnection_.stop();
  lastError_ = message;
  lastStatusCode_ = statusCode;
  Serial.printf("[Drive] %s (HTTP %d)\n", message.c_str(), statusCode);
  return false;
}

void GDriveClient::endSession() {
  driveConnection_.stop();
  validatedFolder_ = "";
  folderValidatedAt_ = 0;
}

void GDriveClient::bindCredentials(const Settings &cfg) {
  if (credentialGeneration_ == cfg.driveAuthGeneration &&
      credentialClientId_ == cfg.driveClientId && credentialSecret_ == cfg.driveClientSecret &&
      credentialRefresh_ == cfg.driveRefreshToken) return;
  endSession();
  cachedAccessToken_ = "";
  accessTokenDeadline_ = 0;
  credentialGeneration_ = cfg.driveAuthGeneration;
  credentialClientId_ = cfg.driveClientId;
  credentialSecret_ = cfg.driveClientSecret;
  credentialRefresh_ = cfg.driveRefreshToken;
}

bool GDriveClient::refreshAccessToken(Settings &cfg, String &outAccessToken, bool force) {
  bindCredentials(cfg);
  if (!force && !cachedAccessToken_.isEmpty() &&
      static_cast<int32_t>(accessTokenDeadline_ - millis()) > 0) {
    outAccessToken = cachedAccessToken_;
    return true;
  }
  cachedAccessToken_ = "";
  accessTokenDeadline_ = 0;
  String body = "client_id=" + urlEncode(cfg.driveClientId) +
                "&client_secret=" + urlEncode(cfg.driveClientSecret) +
                "&refresh_token=" + urlEncode(cfg.driveRefreshToken) + "&grant_type=refresh_token";
  HttpResponse response;
  bool ok = oauthPost("/token", body, response);
  lastStatusCode_ = response.statusCode;
  if (!ok || response.statusCode != 200) return fail(oauthProblem(response, ok), response.statusCode);
  Json root(parseJson(response.body));
  String type, token;
  if (!root.p || !getString(root.p, "access_token", token, 2048) ||
      !getString(root.p, "token_type", type, 32) || !type.equalsIgnoreCase("Bearer")) {
    return fail("Resposta OAuth invalida ou access token maior que 2048 bytes", response.statusCode);
  }
  // Fixtures/servidores legados sem expires_in sao aceitos, sem cache.
  // TTL sempre menor que 2^31 ms, permitindo wrap seguro de millis().
  uint32_t ttl = tokenLifetimeMs(root.p);
  if (ttl) {
    cachedAccessToken_ = token;
    accessTokenDeadline_ = millis() + ttl;
  }
  outAccessToken = std::move(token);
  lastError_ = "";
  return true;
}

bool GDriveClient::pairDevice(SettingsStore &settingsStore, ShowCodeFn showCode, ServiceFn service) {
  endSession();
  cachedAccessToken_ = "";
  accessTokenDeadline_ = 0;
  SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "pareamento Drive");
  lastError_ = "";
  lastStatusCode_ = 0;
  Settings &cfg = settingsStore.get();
  if (!cfg.driveClientId[0]) return fail("Client ID OAuth nao configurado", 0);
  if (service && !service()) return fail("Pareamento cancelado", 0);
  String body = "client_id=" + urlEncode(cfg.driveClientId) +
                "&scope=" + urlEncode("https://www.googleapis.com/auth/drive.file");
  HttpResponse response;
  uint32_t started = millis();
  bool ok = oauthPost("/device/code", body, response);
  lastStatusCode_ = response.statusCode;
  if (service && !service()) return fail("Pareamento cancelado", response.statusCode);
  if (!ok || response.statusCode != 200) return fail(oauthProblem(response, ok), response.statusCode);
  Json root(parseJson(response.body));
  String deviceCode, userCode, verificationUrl;
  uint64_t expires = 0, interval = 5;
  if (!root.p || !getString(root.p, "device_code", deviceCode, 2048) ||
      !getString(root.p, "user_code", userCode, 128) ||
      (!getString(root.p, "verification_url", verificationUrl, 2048) &&
       !getString(root.p, "verification_uri", verificationUrl, 2048)) ||
      !getUnsigned(root.p, "expires_in", INT32_MAX / 1000, expires) || !expires ||
      (item(root.p, "interval") && !getUnsigned(root.p, "interval", INT32_MAX / 1000, interval)) || !interval) {
    return fail("Resposta de pareamento OAuth invalida", response.statusCode);
  }
  if (showCode) showCode(userCode.c_str(), verificationUrl.c_str());
  body = "client_id=" + urlEncode(cfg.driveClientId) +
         "&client_secret=" + urlEncode(cfg.driveClientSecret) +
         "&device_code=" + urlEncode(deviceCode) +
         "&grant_type=" + urlEncode("urn:ietf:params:oauth:grant-type:device_code");
  uint32_t lifetime = static_cast<uint32_t>(expires * 1000);
  uint32_t pollInterval = static_cast<uint32_t>(interval * 1000);
  while (static_cast<uint32_t>(millis() - started) < lifetime) {
    uint32_t waitStarted = millis();
    SyncTelemetry::phase(SyncTelemetry::Phase::RetryWait, "aguardando autorizacao");
    while (static_cast<uint32_t>(millis() - waitStarted) < pollInterval) {
      if (service && !service()) return fail("Pareamento cancelado", lastStatusCode_);
      if (static_cast<uint32_t>(millis() - started) >= lifetime) return fail("Codigo de pareamento expirado", lastStatusCode_);
      delay(25);
    }
    if (service && !service()) return fail("Pareamento cancelado", lastStatusCode_);
    SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "consultando autorizacao");
    HttpResponse poll;
    ok = oauthPost("/token", body, poll);
    lastStatusCode_ = poll.statusCode;
    if (service && !service()) return fail("Pareamento cancelado", poll.statusCode);
    // Nao aceitar uma autorizacao cuja resposta chegou depois do prazo.
    if (static_cast<uint32_t>(millis() - started) >= lifetime) return fail("Codigo de pareamento expirado", poll.statusCode);
    if (!ok) { pollInterval = pollInterval < 60000 ? 60000 : pollInterval; continue; }
    Json result(parseJson(poll.body));
    if (poll.statusCode == 200) {
      String refresh, access, type;
      if (!result.p || !getString(result.p, "refresh_token", refresh, 512) ||
          refresh.length() >= sizeof(cfg.driveRefreshToken) ||
          !getString(result.p, "access_token", access, 2048) ||
          !getString(result.p, "token_type", type, 32) || !type.equalsIgnoreCase("Bearer")) {
        return fail("Resposta OAuth invalida ou token excede a capacidade de Settings", poll.statusCode);
      }
      if (!settingsStore.saveDriveRefreshToken(refresh.c_str())) return fail("Falha ao salvar autorizacao OAuth em NVS", poll.statusCode);
      bindCredentials(cfg);
      uint32_t ttl = tokenLifetimeMs(result.p);
      if (ttl) { cachedAccessToken_ = access; accessTokenDeadline_ = millis() + ttl; }
      lastError_ = "";
      SyncTelemetry::phase(SyncTelemetry::Phase::Done, "Drive pareado");
      return true;
    }
    String error;
    if (result.p && getString(result.p, "error", error, 128)) {
      if (error == "authorization_pending") continue;
      if (error == "slow_down") {
        if (pollInterval <= static_cast<uint32_t>(INT32_MAX) - 5000) pollInterval += 5000;
        continue;
      }
    }
    if (HttpClient::isRetryableStatus(poll.statusCode, poll.body)) {
      uint32_t wait = poll.retryAfterSec > 60 ? UINT32_MAX : HttpClient::retryDelayMs(0, poll.retryAfterSec);
      if (wait == UINT32_MAX || wait > 60000) return fail("Pareamento adiado pelo servidor; tente mais tarde", poll.statusCode);
      if (wait > pollInterval) pollInterval = wait;
      continue;
    }
    return fail(oauthProblem(poll, true), poll.statusCode);
  }
  return fail("Codigo de pareamento expirado", lastStatusCode_);
}

struct GDriveClient::Job {
  enum class Remote { Unknown, Missing, Match, Different, Foreign, Error };
  struct Snapshot {
    String path;
    String hash;
    size_t size = 0;
    ContentGuard guard;
    bool valid = false;
  } snapshots[3];
  Remote inspected[3] = {Remote::Unknown, Remote::Unknown, Remote::Unknown};
  bool freshReservation[3] = {};
  GDriveClient &owner;
  SettingsStore &store;
  Settings &cfg;
  NoteSyncState &state;
  const char *wavPath;
  SyncProgressFn callback;
  String token;
  String folder;
  bool refreshed401 = false;
  bool persistenceFailed = false;
  String authFailure;
  int authFailureStatus = 0;
  size_t transferCurrent = 0, transferTotal = 0;
  String transferName;

  Job(GDriveClient &client, SettingsStore &settings, NoteSyncState &sync,
      const char *path, SyncProgressFn progress)
      : owner(client), store(settings), cfg(settings.get()), state(sync),
        wavPath(path), callback(progress) {}

  void progress(SyncStage stage, const char *name, size_t done, size_t total,
                int attempt, const char *message) {
    SyncTelemetry::Phase phase = SyncTelemetry::Phase::Preparing;
    if (stage == SyncStage::UploadingWav || stage == SyncStage::UploadingTxt || stage == SyncStage::UploadingMd)
      phase = SyncTelemetry::Phase::DriveUpload;
    else if (stage == SyncStage::VerifyingChecksum) phase = SyncTelemetry::Phase::Verifying;
    else if (stage == SyncStage::Done) phase = SyncTelemetry::Phase::Done;
    else if (stage == SyncStage::Error) phase = SyncTelemetry::Phase::Error;
    const char *detail = phase == SyncTelemetry::Phase::DriveUpload && name && *name
                             ? name : (message ? message : "");
    SyncTelemetry::transfer(phase, done, total, detail);
    if (!callback) return;
    SyncProgress p;
    p.stage = stage;
    p.fileName = name ? name : "";
    p.bytesUploaded = done;
    p.totalBytes = total;
    p.attempt = attempt;
    p.maxAttempts = kMaxAttempts;
    p.message = message;
    callback(p); // Strings validas durante a callback, como na API anterior.
  }

  void beginTransfer(SyncStage stage, const char *name, size_t offset, size_t total, int attempt) {
    transferCurrent = offset;
    transferTotal = total;
    transferName = name ? name : "";
    progress(stage, name, offset, total, attempt, "enviando arquivo");
  }

  static void wrote(size_t count, void *context) {
    Job &job = *static_cast<Job *>(context);
    SyncTelemetry::sentBytes(SyncTelemetry::Phase::DriveUpload, count);
    job.transferCurrent += count;
    // So o observer barato de telemetria recebe cada write; a callback publica
    // conserva os eventos de etapa/ACK, sem obrigar UI legada a desenhar 1 KiB.
    SyncTelemetry::transfer(SyncTelemetry::Phase::DriveUpload, job.transferCurrent,
                            job.transferTotal, job.transferName.c_str());
  }

  bool persist() {
    if (persistenceFailed) return false;
    state.lastStatusCode = owner.lastStatusCode_;
    state.lastAttemptTime = millis();
    diagnostic(state.lastError, sizeof(state.lastError), owner.lastError_);
    if (state.save(wavPath)) return true;
    persistenceFailed = true;
    owner.fail("Falha ao salvar estado; sincronizacao interrompida antes da proxima mutacao", owner.lastStatusCode_);
    progress(SyncStage::Error, wavPath, 0, 0, 0, owner.lastError_.c_str());
    return false;
  }

  bool error(const String &message, int code) {
    if (persistenceFailed) return false;
    state.fullySynced = false;
    owner.fail(message, code);
    persist();
    progress(SyncStage::Error, wavPath, 0, 0, 0, owner.lastError_.c_str());
    return false;
  }

  bool httpError(const char *operation, const HttpResponse &r, bool ok) {
    if (persistenceFailed) return false;
    if (!authFailure.isEmpty()) return error(authFailure, authFailureStatus);
    return error(httpProblem(operation, r, ok), r.statusCode);
  }

  bool request(const char *method, const String &path, const String &body,
               const String &extra, HttpResponse &r, const String &session = "",
               File *file = nullptr, size_t offset = 0, size_t length = 0,
               const String &suffix = "") {
    if (persistenceFailed) { r = HttpResponse(); return false; }
    if (!owner.cachedAccessToken_.isEmpty() &&
        static_cast<int32_t>(owner.accessTokenDeadline_ - millis()) <= 0) {
      if (!owner.refreshAccessToken(cfg, token)) {
        authFailure = owner.lastError_; authFailureStatus = owner.lastStatusCode_;
        r = HttpResponse(); return false;
      }
    }
    String host("www.googleapis.com"), target(path);
    uint16_t port = 443;
    if (!session.isEmpty() && !sessionParts(session, host, port, target)) {
      r = HttpResponse();
      owner.fail("URL de sessao invalida; estado preservado para recuperacao", 0);
      return false;
    }
    // Um unico contexto Drive, normalmente www.googleapis.com. Uma Location
    // permitida em outro host muda o destino e fecha o socket anterior; jamais
    // reutilizar TLS/Bearer num host/porta diferente sem um novo connect.
    HttpClient::Connection *connection = &owner.driveConnection_;
    bool ok = transport(host, port, target, method, token, extra, body, r, file, offset, length,
                        connection, suffix, file ? wrote : nullptr, this);
    if (r.statusCode == 404 || r.statusCode == 403) owner.validatedFolder_ = "";
    // Somente GET e probe vazio podem ser repetidos numa conexao nova.
    // Mutacoes seguem inspect/probe no chamador, sempre apos reconnect.
    bool safeReplay = String(method) == "GET" ||
                      (!file && String(method) == "PUT" && body.isEmpty());
    if (!ok && r.statusCode != 401 && safeReplay) {
      if (connection) connection->stop();
      ok = transport(host, port, target, method, token, extra, body, r,
                     nullptr, 0, 0, connection);
    }
    owner.lastStatusCode_ = r.statusCode;
    if (r.statusCode == 404 || r.statusCode == 403) owner.validatedFolder_ = "";
    if (r.statusCode == 401) {
      owner.cachedAccessToken_ = "";
      owner.accessTokenDeadline_ = 0;
      owner.driveConnection_.stop();
    }
    if (r.statusCode == 401 && !refreshed401) {
      refreshed401 = true; // Uma unica renovacao reativa para todo o job.
      if (!owner.refreshAccessToken(cfg, token, true)) {
        authFailure = owner.lastError_;
        authFailureStatus = owner.lastStatusCode_;
        return false;
      }
      if (!persist()) return false;
      // Nem bytes nem metadados de criacao sao repetidos sem inspect/probe.
      // GET e PUT vazio (probe) podem ser repetidos diretamente apos 401.
      if (file || String(method) == "POST" || String(method) == "PATCH") {
        owner.lastStatusCode_ = r.statusCode; return ok;
      }
      ok = transport(host, port, target, method, token, extra, body, r,
                     nullptr, 0, 0, connection);
      owner.lastStatusCode_ = r.statusCode;
      if (r.statusCode == 401) {
        owner.cachedAccessToken_ = "";
        owner.accessTokenDeadline_ = 0;
        owner.driveConnection_.stop();
      }
      if (r.statusCode == 404 || r.statusCode == 403) owner.validatedFolder_ = "";
    }
    return ok;
  }

  bool generateIds(unsigned count, String *out) {
    if (!count || count > 3) return error("Quantidade de IDs reservados invalida", 0);
    HttpResponse r;
    bool ok = request("GET", "/drive/v3/files/generateIds?count=" + String(count) +
                      "&space=drive&type=files&fields=ids", "", "", r);
    if (!ok || r.statusCode != 200) return httpError("Reservar ID no Drive", r, ok);
    Json root(parseJson(r.body));
    cJSON *ids = root.p ? item(root.p, "ids") : nullptr;
    if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) != static_cast<int>(count))
      return error("Drive retornou lote de IDs reservado invalido", r.statusCode);
    for (unsigned i = 0; i < count; ++i) {
      cJSON *entry = cJSON_GetArrayItem(ids, i);
      if (!cJSON_IsString(entry) || !plainString(entry->valuestring) || !isId(entry->valuestring))
        return error("Drive retornou ID reservado invalido", r.statusCode);
      out[i] = entry->valuestring;
      for (unsigned j = 0; j < i; ++j) {
        if (out[j] == out[i]) return error("Drive retornou IDs reservados repetidos", r.statusCode);
      }
    }
    return true;
  }

  bool generateId(String &id) { return generateIds(1, &id); }

  // Paginacao completa; resposta parcial/limite nunca permite criar duplicata.
  bool list(const String &query, const char *fields, String &pageToken, HttpResponse &r) {
    String path = "/drive/v3/files?q=" + urlEncode(query) + "&spaces=drive&pageSize=20&fields=" + urlEncode(fields);
    if (!pageToken.isEmpty()) path += "&pageToken=" + urlEncode(pageToken);
    bool ok = request("GET", path, "", "", r);
    if (!ok || r.statusCode != 200) return httpError("Consultar arquivos existentes", r, ok);
    return true;
  }

  bool validateFolder(const String &id, bool &valid, bool *missing = nullptr) {
    valid = false;
    if (missing) *missing = false;
    HttpResponse r;
    bool ok = request("GET", "/drive/v3/files/" + urlEncode(id) + "?fields=" + urlEncode(kFolderFields), "", "", r);
    if (ok && r.statusCode == 404) {
      if (missing) *missing = true;
      return true;
    }
    if (!ok || r.statusCode != 200) return httpError("Validar pasta do Drive", r, ok);
    Json root(parseJson(r.body));
    String actual, mime;
    bool trashed;
    if (!root.p || !getString(root.p, "id", actual, 127) || actual != id ||
        !getString(root.p, "mimeType", mime, 128) || !boolField(root.p, "trashed", trashed)) {
      return error("Metadados da pasta incompletos ou invalidos", r.statusCode);
    }
    valid = !trashed && mime == kFolderMime;
    if (valid) {
      owner.validatedFolder_ = id;
      owner.folderValidatedAt_ = millis();
    } else owner.validatedFolder_ = "";
    return true;
  }

  bool saveFolder(const String &id) {
    if (!isId(id) || id.length() >= sizeof(cfg.driveFolderId)) return error("ID da pasta excede a capacidade de Settings", owner.lastStatusCode_);
    if (id != cfg.driveFolderId && !store.saveDriveFolderId(id.c_str())) return error("Falha ao persistir ID da pasta em NVS", owner.lastStatusCode_);
    folder = id;
    return true;
  }

  bool confirmFolder(const String &id) {
    if (!saveFolder(id)) return false;
    state.pendingFolderId[0] = '\0';
    return persist();
  }

  bool ensureFolder() {
    progress(SyncStage::CheckingFolder, "", 0, 0, 1, "validando pasta do Drive");
    bool valid;
    String previousReservation;
    String cached = state.pendingFolderId[0] ? String(state.pendingFolderId) : String(cfg.driveFolderId);
    if (!state.pendingFolderId[0] && !cached.isEmpty() && owner.validatedFolder_ == cached &&
        static_cast<uint32_t>(millis() - owner.folderValidatedAt_) < kFolderCacheMs)
      return confirmFolder(cached);
    if (!cached.isEmpty()) {
      if (!isId(cached)) return error("ID da pasta salvo e invalido", 0);
      bool missing;
      if (!validateFolder(cached, valid, &missing)) return false;
      if (valid) return confirmFolder(cached);
      // Diferenciar pasta confirmada que foi apagada de reserva que sofreu
      // timeout: so uma reserva persistida e repetida com o mesmo ID.
      if (missing && cached == state.pendingFolderId) previousReservation = cached;
    }
    String query = "trashed = false and mimeType = " + queryLiteral(kFolderMime) + " and name = " + queryLiteral(kFolderName);
    String page;
    unsigned pages = 0;
    do {
      HttpResponse r;
      if (!list(query, "nextPageToken,files(id,name,mimeType,trashed)", page, r)) return false;
      Json root(parseJson(r.body));
      cJSON *files = root.p ? item(root.p, "files") : nullptr;
      if (!cJSON_IsArray(files)) return error("Resposta de listagem de pastas invalida", r.statusCode);
      cJSON *entry;
      cJSON_ArrayForEach(entry, files) {
        String id, mime, name;
        bool trashed;
        if (!getString(entry, "id", id, 127) || !isId(id) ||
            !getString(entry, "mimeType", mime, 128) || !getString(entry, "name", name, 1024) ||
            !boolField(entry, "trashed", trashed)) return error("Metadados de pasta incompletos", r.statusCode);
        if (!trashed && mime == kFolderMime && name == kFolderName) {
          if (!validateFolder(id, valid)) return false;
          if (valid) return confirmFolder(id);
        }
      }
      page = "";
      if (item(root.p, "nextPageToken") && !getString(root.p, "nextPageToken", page, 2048)) return error("Paginacao de pastas invalida", r.statusCode);
      if (++pages > 100) return error("Listagem de pastas excedeu o limite; nenhuma pasta criada", r.statusCode);
    } while (!page.isEmpty());

    // Reutilizar uma reserva que sofreu timeout: GET antes de novo POST.
    // O ID gerado e salvo em Settings ANTES da criacao da pasta.
    String id(previousReservation);
    if (id.isEmpty() && !generateId(id)) return false;
    if (!copyString(state.pendingFolderId, sizeof(state.pendingFolderId), id) ||
        !persist() || !saveFolder(id) || !persist()) return false;
    Json meta(cJSON_CreateObject());
    if (!meta.p || !cJSON_AddStringToObject(meta.p, "id", id.c_str()) ||
        !cJSON_AddStringToObject(meta.p, "name", kFolderName) ||
        !cJSON_AddStringToObject(meta.p, "mimeType", kFolderMime)) return error("Sem memoria para metadados da pasta", 0);
    String body = render(meta.p);
    if (body.isEmpty()) return error("Sem memoria para criar pasta", 0);
    HttpResponse r;
    bool authWasRefreshed = refreshed401;
    bool ok = request("POST", "/drive/v3/files?fields=" + urlEncode(kFolderFields), body,
                      "Content-Type: application/json; charset=UTF-8\r\n", r);
    if (persistenceFailed) return false;
    if (r.statusCode == 401 && !authWasRefreshed && refreshed401 && authFailure.isEmpty()) {
      bool missing;
      if (!validateFolder(id, valid, &missing)) return false;
      if (valid) return confirmFolder(id);
      if (!missing) return error("Reserva de pasta nao corresponde a uma pasta valida", owner.lastStatusCode_);
      // O 404 foi inspecionado com o token novo antes do unico novo POST.
      if (!persist()) return false;
      ok = request("POST", "/drive/v3/files?fields=" + urlEncode(kFolderFields), body,
                   "Content-Type: application/json; charset=UTF-8\r\n", r);
      if (persistenceFailed) return false;
    }
    if (!ok || (!accepted(r.statusCode) && r.statusCode != 409)) {
      // Criacao ambigua: verificar a reserva, nunca criar outra neste job.
      int code = r.statusCode;
      if (!ok || HttpClient::isRetryableStatus(code, r.body)) {
        owner.driveConnection_.stop();
        if (!validateFolder(id, valid)) return false;
        if (valid) return confirmFolder(id);
      }
      return httpError("Criar pasta do Drive", r, ok);
    }
    if (!validateFolder(id, valid)) return false;
    if (!valid) return error("Pasta criada nao foi confirmada no Drive", owner.lastStatusCode_);
    return confirmFolder(id);
  }

  Remote metadata(const cJSON *root, const Asset &a, const char *name) {
    String id, remoteName, hash, sizeString;
    bool trashed;
    size_t size;
    if (!root || !getString(root, "id", id, 127)) return Remote::Unknown;
    if (id != a.id) return Remote::Foreign;
    if (!getString(root, "name", remoteName, 1024) || !boolField(root, "trashed", trashed) ||
        !cJSON_IsArray(item(root, "parents"))) return Remote::Unknown;
    if (trashed || !hasParent(root, folder) || remoteName != name) return Remote::Foreign;
    if (!getString(root, "size", sizeString, 32) || !decimal(sizeString, size) ||
        !getString(root, "md5Checksum", hash, 32) || !isMd5(hash)) return Remote::Unknown;
    return size == a.size && hash.equalsIgnoreCase(a.hash) ? Remote::Match : Remote::Different;
  }

  Remote inspect(const Asset &a, const char *name) {
    SyncTelemetry::phase(SyncTelemetry::Phase::Verifying, "verificando arquivo remoto");
    HttpResponse r;
    bool ok = request("GET", "/drive/v3/files/" + urlEncode(a.id) + "?fields=" + urlEncode(kFileFields), "", "", r);
    if (ok && r.statusCode == 404) return Remote::Missing;
    if (!ok || r.statusCode != 200) { httpError("Verificar arquivo remoto", r, ok); return Remote::Error; }
    Json root(parseJson(r.body));
    Remote result = metadata(root.p, a, name);
    if (result == Remote::Unknown) {
      error("GET do arquivo nao retornou metadados, tamanho e MD5 verificaveis", r.statusCode);
      return Remote::Error;
    }
    return result;
  }

  bool localUnchanged(Asset a, const char *path, bool text) {
    SyncTelemetry::phase(SyncTelemetry::Phase::Verifying, "validando fotografia local");
    const Snapshot *snapshot = nullptr;
    for (const Snapshot &candidate : snapshots) {
      if (candidate.valid && candidate.path == path) { snapshot = &candidate; break; }
    }
    bool valid = snapshot && snapshot->size == a.size && snapshot->hash.equalsIgnoreCase(a.hash) &&
                 (text ? NoteFiles::validText(path) : NoteFiles::validWav(path));
    File file = valid ? NoteFiles::openRead(path) : File();
    ContentGuard guard;
    size_t remaining = a.size;
    uint8_t buffer[1024];
    valid = valid && file && !file.isDirectory() && file.size() == a.size;
    while (valid && remaining) {
      size_t wanted = min(remaining, sizeof(buffer));
      size_t n = file.read(buffer, wanted);
      if (!n || n > wanted) { valid = false; break; }
      guard.add(buffer, n);
      remaining -= n;
      yield();
    }
    valid = valid && file.size() == a.size && guard.equals(snapshot->guard);
    if (!valid) {
      a.uploaded = false;
      return error("Arquivo local ausente, invalido ou alterado durante a sincronizacao", owner.lastStatusCode_);
    }
    return true;
  }

  bool complete(Asset a, const char *path, const char *name, SyncStage stage, bool text,
                Remote verified = Remote::Unknown, const HttpResponse *finalResponse = nullptr) {
    progress(SyncStage::VerifyingChecksum, name, a.size, a.size, 1, "confirmando tamanho e MD5");
    a.uploaded = false;
    // ACK final (ou GET correspondente) confirma os bytes, mas ainda NAO o
    // checksum. Persistir inclusive o ultimo bloco antes da verificacao.
    a.offset = a.size;
    if (!persist()) return false;
    if (!localUnchanged(a, path, text)) return false;
    Remote remote = verified;
    if (remote == Remote::Unknown && finalResponse && finalResponse->bodyComplete &&
        !finalResponse->bodyTruncated && accepted(finalResponse->statusCode)) {
      Json root(parseJson(finalResponse->body));
      remote = metadata(root.p, a, name);
    }
    // Um ACK com todos os fields ou um inspect ja validado elimina o GET.
    // JSON vazio/incompleto nunca comprova conclusao: GET conservador.
    if (remote == Remote::Unknown) remote = inspect(a, name);
    if (remote == Remote::Error) return false;
    if (remote == Remote::Different) {
      // Uma sessao que terminou com bytes divergentes nao pode ficar sendo
      // consultada eternamente. Manter o ID e permitir PATCH no proximo job.
      a.session = "";
      a.offset = 0;
      a.reserved = false;
      return error("Tamanho ou MD5 remoto divergente; reenvio pendente no mesmo ID", owner.lastStatusCode_);
    }
    if (remote != Remote::Match) return error("Arquivo remoto nao corresponde ao arquivo local; conclusao recusada", owner.lastStatusCode_);
    a.uploaded = true;
    a.reserved = false;
    a.offset = a.size;
    a.session = "";
    if (!persist()) return false;
    progress(stage, name, a.offset, a.size, 1, "arquivo verificado");
    return true;
  }

  bool reconcile(Asset a, const char *name) {
    SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "reconciliando arquivo");
    String query = "trashed = false and " + queryLiteral(folder) + " in parents and name = " + queryLiteral(name);
    String page;
    unsigned pages = 0;
    do {
      HttpResponse r;
      if (!list(query, "nextPageToken,files(id,name,size,md5Checksum,trashed,parents,mimeType)", page, r)) return false;
      Json root(parseJson(r.body));
      cJSON *files = root.p ? item(root.p, "files") : nullptr;
      if (!cJSON_IsArray(files)) return error("Resposta de reconciliacao invalida", r.statusCode);
      cJSON *entry;
      cJSON_ArrayForEach(entry, files) {
        String id, remoteName, hash, sizeString;
        bool trashed;
        size_t size;
        if (!getString(entry, "id", id, 127) || !isId(id) ||
            !getString(entry, "name", remoteName, 1024) || !boolField(entry, "trashed", trashed)) return error("Metadados incompletos na reconciliacao", r.statusCode);
        if (trashed || remoteName != name || !hasParent(entry, folder)) continue;
        if (!getString(entry, "size", sizeString, 32) || !decimal(sizeString, size) ||
            !getString(entry, "md5Checksum", hash, 32) || !isMd5(hash)) {
          return error("Arquivo de mesmo nome sem tamanho/MD5; reconciliacao interrompida", r.statusCode);
        }
        if (size == a.size && hash.equalsIgnoreCase(a.hash)) {
          copyString(a.id, 128, id);
          a.reserved = false;
          a.uploaded = false;
          a.session = "";
          a.offset = 0;
          return persist(); // Ainda exige GET direto antes de concluir.
        }
      }
      page = "";
      if (item(root.p, "nextPageToken") && !getString(root.p, "nextPageToken", page, 2048)) return error("Paginacao de reconciliacao invalida", r.statusCode);
      if (++pages > 100) return error("Reconciliacao excedeu limite; nenhum arquivo criado", r.statusCode);
    } while (!page.isEmpty());
    return true;
  }

  int indexOf(const Asset &a) const {
    return a.id == state.wavDriveId ? 0 : a.id == state.txtDriveId ? 1 : 2;
  }

  bool prepareAssets(const bool *present) {
    unsigned missing = 0;
    int slots[3] = {};
    for (int i = 0; i < 3; ++i) {
      if (!present[i]) continue;
      Asset a = asset(state, i);
      String name = baseName(snapshots[i].path.c_str());
      if (name.isEmpty() || !plainString(name.c_str())) return error("Nome de arquivo local invalido", 0);
      if (a.id[0]) {
        inspected[i] = inspect(a, name.c_str());
        if (inspected[i] == Remote::Error) return false;
        if (inspected[i] == Remote::Foreign || (inspected[i] == Remote::Missing && !a.reserved)) {
          a.id[0] = '\0'; a.reserved = false; a.uploaded = false; a.session = ""; a.offset = 0;
          inspected[i] = Remote::Unknown;
          if (!persist()) return false;
        }
      }
      if (!a.id[0]) {
        // Sem UUID/prova de criacao exclusiva nao e seguro omitir esta busca,
        // mesmo para um .sync novo: pode haver um upload legado/estado perdido.
        if (!reconcile(a, name.c_str())) return false;
        if (a.id[0]) {
          inspected[i] = inspect(a, name.c_str());
          if (inspected[i] != Remote::Match)
            return inspected[i] == Remote::Error ? false : error("Arquivo reconciliado mudou antes da confirmacao", owner.lastStatusCode_);
        } else slots[missing++] = i;
      }
    }
    if (!missing) return true;
    SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "reservando IDs do lote");
    String ids[3];
    if (!generateIds(missing, ids)) return false;
    for (unsigned n = 0; n < missing; ++n) {
      int i = slots[n];
      // Tambem rejeitar um lote que colide com qualquer ID local conhecido.
      for (int j = 0; j < 3; ++j) {
        if (ids[n] == asset(state, j).id || ids[n] == folder || ids[n] == state.pendingFolderId)
          return error("ID reservado colide com recurso conhecido", owner.lastStatusCode_);
      }
      Asset a = asset(state, i);
      if (!copyString(a.id, 128, ids[n])) return error("ID reservado excede a capacidade", owner.lastStatusCode_);
      a.uploaded = false; a.reserved = true; a.session = ""; a.offset = 0;
      freshReservation[i] = true;
      inspected[i] = Remote::Unknown;
    }
    // Todos os IDs juntos no .sync antes de qualquer POST de arquivo.
    return persist();
  }

  enum class Probe { Continue, Complete, Restart, Error };

  Probe probe(Asset a, const char *path, const char *name, SyncStage stage, bool text) {
    progress(stage, name, a.offset, a.size, 1, "consultando sessao de upload");
    HttpResponse r;
    String extra = "Content-Range: bytes */" + String(static_cast<unsigned long>(a.size)) + "\r\n";
    bool ok = request("PUT", "", "", extra, r, a.session);
    if (persistenceFailed) return Probe::Error;
    if (!ok) { httpError("Consultar sessao (preservada para retomada)", r, ok); return Probe::Error; }
    if (accepted(r.statusCode)) return complete(a, path, name, stage, text, Remote::Unknown, &r) ? Probe::Complete : Probe::Error;
    if (r.statusCode == 308) {
      size_t next;
      if (!rangeOffset(r.range, a.size, a.offset, a.size, next)) {
        error("Range de sessao invalido ou regressivo; progresso preservado", r.statusCode);
        return Probe::Error;
      }
      a.offset = next;
      if (!persist()) return Probe::Error;
      progress(stage, name, a.offset, a.size, 1, "progresso confirmado pelo servidor");
      if (next == a.size) return complete(a, path, name, stage, text) ? Probe::Complete : Probe::Error;
      return Probe::Continue;
    }
    if (r.statusCode == 404) {
      // Sessao pode ter expirado DEPOIS de concluir. Verificar o mesmo ID.
      Remote remote = inspect(a, name);
      if (remote == Remote::Error) return Probe::Error;
      if (remote == Remote::Match) return complete(a, path, name, stage, text, remote) ? Probe::Complete : Probe::Error;
      if (remote == Remote::Foreign || (remote == Remote::Missing && !a.reserved)) {
        error("Arquivo remoto movido ou removido; retomada recusada", owner.lastStatusCode_);
        return Probe::Error;
      }
      inspected[indexOf(a)] = remote;
      a.session = "";
      a.offset = 0;
      a.uploaded = false;
      if (!persist()) return Probe::Error;
      return Probe::Restart;
    }
    httpError("Consultar sessao (preservada para retomada)", r, true);
    return Probe::Error;
  }

  bool initiate(Asset a, const char *path, const char *name, const char *mime,
                 SyncStage stage, bool text) {
    int index = indexOf(a);
    Remote remote = freshReservation[index] ? Remote::Missing : inspected[index];
    // Missing e uma reserva fresca ou um GET 404 deste job. A primeira
    // mutacao consome essa prova; falhas posteriores sempre fazem inspect.
    if (remote == Remote::Unknown) remote = inspect(a, name);
    if (remote == Remote::Error) return false;
    if (remote == Remote::Match) return complete(a, path, name, stage, text, remote);
    if (remote == Remote::Foreign) return error("ID remoto fora da pasta, renomeado ou na lixeira; reconciliacao necessaria", owner.lastStatusCode_);
    bool update = remote == Remote::Different;
    SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "preparando sessao resumable");
    Json meta(cJSON_CreateObject());
    if (!meta.p || !cJSON_AddStringToObject(meta.p, "name", name) ||
        !cJSON_AddStringToObject(meta.p, "mimeType", mime)) return error("Sem memoria para metadados do upload", 0);
    if (!update) {
      cJSON *parents = cJSON_AddArrayToObject(meta.p, "parents");
      cJSON *parent = cJSON_CreateString(folder.c_str());
      if (!cJSON_AddStringToObject(meta.p, "id", a.id) || !parents || !parent) {
        cJSON_Delete(parent);
        return error("Sem memoria para ID/pasta do upload", 0);
      }
      // cJSON do ESP-IDF 4.x retorna void aqui; versoes novas retornam bool.
      cJSON_AddItemToArray(parents, parent);
      if (cJSON_GetArrayItem(parents, 0) != parent) {
        cJSON_Delete(parent);
        return error("Falha ao montar pasta do upload", 0);
      }
    }
    String body = render(meta.p);
    if (body.isEmpty()) return error("Sem memoria para iniciar upload", 0);
    if (!localUnchanged(a, path, text) || !persist()) return false;
    String target = "/upload/drive/v3/files";
    if (update) target += "/" + urlEncode(a.id);
    target += "?uploadType=resumable&fields=" + urlEncode(kFileFields);
    String extra = "Content-Type: application/json; charset=UTF-8\r\nX-Upload-Content-Type: " + String(mime) +
                   "\r\nX-Upload-Content-Length: " + String(static_cast<unsigned long>(a.size)) + "\r\n";
    HttpResponse r;
    freshReservation[index] = false;
    inspected[index] = Remote::Unknown;
    progress(stage, name, 0, a.size, 1, "iniciando sessao de upload");
    bool authWasRefreshed = refreshed401;
    bool ok = request(update ? "PATCH" : "POST", target, body, extra, r);
    if (persistenceFailed) return false;
    if (!ok || r.statusCode == 409 || !accepted(r.statusCode)) {
      // Sem Location em uma resposta ambigua: GET da reserva antes de qualquer
      // proximo POST. Novo job reutiliza o ID, inclusive apos 409.
      int originalStatus = r.statusCode;
      bool renewed = originalStatus == 401 && !authWasRefreshed && refreshed401 && authFailure.isEmpty();
      if (!ok || renewed || originalStatus == 409 || HttpClient::isRetryableStatus(originalStatus, r.body)) {
        owner.driveConnection_.stop();
        Remote check = inspect(a, name);
        if (check == Remote::Error) return false;
        if (check == Remote::Match) return complete(a, path, name, stage, text, check);
        if (renewed && (check == Remote::Different || (check == Remote::Missing && a.reserved))) {
          inspected[index] = check;
          // Profundidade maxima dois: refreshed401 ja impede outra renovacao.
          return initiate(a, path, name, mime, stage, text);
        }
      }
      return httpError("Iniciar upload; ID reservado preservado", r, ok);
    }
    String host, sessionPath;
    uint16_t port;
    if (!sessionParts(r.location, host, port, sessionPath)) return error("Location ausente, invalida ou maior que o limite HTTP; ID preservado", r.statusCode);
    a.session = std::move(r.location); // Inteira, para WAV, TXT e MD.
    a.offset = 0;
    a.uploaded = false;
    return persist(); // Nunca enviar dados antes de persistir a sessao.
  }

  bool multipart(int index, const char *path, const char *name, const char *mime,
                 SyncStage stage, bool text) {
    Asset a = asset(state, index);
    Remote remote = freshReservation[index] ? Remote::Missing : inspected[index];
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      if (remote == Remote::Unknown) remote = inspect(a, name);
      if (remote == Remote::Error) return false;
      if (remote == Remote::Match) return complete(a, path, name, stage, text, remote);
      if (remote == Remote::Foreign || (remote == Remote::Missing && !a.reserved))
        return error("ID remoto movido, renomeado ou removido; upload recusado", owner.lastStatusCode_);
      bool update = remote == Remote::Different;
      SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "preparando multipart");
      Json meta(cJSON_CreateObject());
      if (!meta.p || !cJSON_AddStringToObject(meta.p, "name", name) ||
          !cJSON_AddStringToObject(meta.p, "mimeType", mime))
        return error("Sem memoria para metadados multipart", 0);
      if (!update) {
        cJSON *parents = cJSON_AddArrayToObject(meta.p, "parents");
        cJSON *parent = cJSON_CreateString(folder.c_str());
        if (!cJSON_AddStringToObject(meta.p, "id", a.id) || !parents || !parent) {
          cJSON_Delete(parent);
          return error("Sem memoria para ID/pasta multipart", 0);
        }
        cJSON_AddItemToArray(parents, parent);
        if (cJSON_GetArrayItem(parents, 0) != parent) {
          cJSON_Delete(parent);
          return error("Falha ao montar pasta multipart", 0);
        }
      }
      String metadataBody = render(meta.p);
      if (metadataBody.isEmpty()) return error("Sem memoria para upload multipart", 0);
      // O boundary depende do ID reservado, sem nomes/conteudo/credenciais.
      String boundary = "esp32_drive_" + String(a.id).substring(0, 48);
      String prefix = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                      metadataBody + "\r\n--" + boundary + "\r\nContent-Type: " + String(mime) + "\r\n\r\n";
      String suffix = "\r\n--" + boundary + "--\r\n";
      String target = "/upload/drive/v3/files";
      if (update) target += "/" + urlEncode(a.id);
      target += "?uploadType=multipart&fields=" + urlEncode(kFileFields);
      String extra = "Content-Type: multipart/related; boundary=" + boundary + "\r\n";
      if (!localUnchanged(a, path, text) || !persist()) return false;
      File file = NoteFiles::openRead(path);
      if (!file || file.size() != a.size) return error("Arquivo local indisponivel para multipart", 0);
      freshReservation[index] = false;
      inspected[index] = Remote::Unknown;
      beginTransfer(stage, name, 0, a.size, attempt);
      HttpResponse r;
      bool ok = request(update ? "PATCH" : "POST", target, prefix, extra, r,
                        "", &file, 0, a.size, suffix);
      file.close();
      if (persistenceFailed) return false;
      if (ok && accepted(r.statusCode)) return complete(a, path, name, stage, text, Remote::Unknown, &r);
      int originalStatus = r.statusCode;
      bool retryable = !ok || originalStatus == 401 || originalStatus == 409 ||
                       HttpClient::isRetryableStatus(originalStatus, r.body);
      if (!retryable) return httpError("Enviar arquivo multipart", r, ok);
      // Resposta perdida, escrita parcial ou conflito: nunca repetir bytes
      // antes de consultar o MESMO ID num transporte limpo.
      owner.driveConnection_.stop();
      remote = inspect(a, name);
      if (remote == Remote::Error) return false;
      if (remote == Remote::Match) return complete(a, path, name, stage, text, remote);
      if (remote == Remote::Foreign || (remote == Remote::Missing && !a.reserved))
        return error("Arquivo remoto mudou durante recuperacao multipart", owner.lastStatusCode_);
      if (attempt == kMaxAttempts) return httpError("Multipart sem confirmacao; ID preservado", r, ok);
      uint32_t wait = HttpClient::retryDelayMs(attempt - 1, r.retryAfterSec);
      if (wait == UINT32_MAX || wait > 60000)
        return error("Upload adiado pelo Retry-After; ID preservado", originalStatus);
      bool quota = originalStatus == 429 || originalStatus == 403;
      SyncTelemetry::wait(wait, quota, quota ? "limite do Drive" : "retentativa multipart");
    }
    return false;
  }

  bool upload(int index, const char *path, const char *mime, SyncStage stage) {
    Asset a = asset(state, index);
    bool text = index != 0;
    String name = baseName(path);
    if (name.isEmpty() || !plainString(name.c_str())) return error("Nome de arquivo local invalido", 0);
    a.uploaded = false;
    if (!persist()) return false;
    if (!isId(a.id)) return error("Upload sem ID previamente reservado e persistido", 0);
    if (inspected[index] == Remote::Match)
      return complete(a, path, name.c_str(), stage, text, Remote::Match);
    if (!a.session.isEmpty()) {
      Probe p = probe(a, path, name.c_str(), stage, text);
      if (p == Probe::Complete) return true;
      if (p == Probe::Error) return false;
    }
    // Sessoes persistidas continuam resumable, mesmo para um arquivo pequeno.
    if (a.size <= kMultipartBytes && a.session.isEmpty())
      return multipart(index, path, name.c_str(), mime, stage, text);
    int stalled = 0, restarts = 0;
    while (!a.uploaded) {
      if (a.session.isEmpty()) {
        if (++restarts > kMaxAttempts) return error("Sessoes expiraram repetidamente; tente mais tarde", owner.lastStatusCode_);
        if (!initiate(a, path, name.c_str(), mime, stage, text)) return false;
        if (a.uploaded) return true;
      }
      if (a.offset == a.size) return complete(a, path, name.c_str(), stage, text);
      File file = NoteFiles::openRead(path);
      if (!file || file.size() != a.size) return error("Arquivo local indisponivel para enviar bloco", 0);
      size_t start = a.offset;
      size_t length = a.size - start < kChunkBytes ? a.size - start : kChunkBytes;
      beginTransfer(stage, name.c_str(), start, a.size, stalled + 1);
      String extra = "Content-Type: " + String(mime) + "\r\nContent-Range: bytes " +
                     String(static_cast<unsigned long>(start)) + "-" +
                     String(static_cast<unsigned long>(start + length - 1)) + "/" +
                     String(static_cast<unsigned long>(a.size)) + "\r\n";
      HttpResponse r;
      bool ok = request("PUT", "", "", extra, r, a.session, &file, start, length);
      file.close();
      if (persistenceFailed) return false;
      if (ok && accepted(r.statusCode)) return complete(a, path, name.c_str(), stage, text, Remote::Unknown, &r);
      if (ok && r.statusCode == 308) {
        size_t next;
        if (!rangeOffset(r.range, a.size, a.offset, start + length, next)) {
          // Resposta estranha tambem e ambigua: consultar antes de repetir.
          owner.driveConnection_.stop();
          Probe p = probe(a, path, name.c_str(), stage, text);
          if (p == Probe::Complete) return true;
          if (p == Probe::Error) return false;
        } else {
          a.offset = next;
          if (!persist()) return false;
          progress(stage, name.c_str(), next, a.size, stalled + 1, "bloco confirmado");
        }
      } else {
        // Inclusive timeout, escrita parcial, 401 e 5xx: SEMPRE fazer probe.
        int originalStatus = r.statusCode;
        bool retryable = !ok || originalStatus == 401 || originalStatus == 404 ||
                         HttpClient::isRetryableStatus(originalStatus, r.body);
        uint32_t wait = r.retryAfterSec > 60 ? UINT32_MAX : HttpClient::retryDelayMs(stalled, r.retryAfterSec);
        owner.driveConnection_.stop();
        Probe p = probe(a, path, name.c_str(), stage, text);
        if (p == Probe::Complete) return true;
        if (p == Probe::Error) return false;
        if (!retryable) return httpError("Enviar bloco", r, ok);
        if (wait == UINT32_MAX || wait > 60000) return error("Upload adiado pelo Retry-After; sessao preservada", originalStatus);
        bool quota = originalStatus == 429 || originalStatus == 403;
        SyncTelemetry::wait(wait, quota, quota ? "limite do Drive" : "retentativa resumable");
      }
      if (a.session.isEmpty()) { stalled = 0; continue; }
      if (a.offset > start) stalled = 0;
      else if (++stalled >= kMaxAttempts) return error("Servidor nao confirmou progresso apos quatro tentativas; sessao preservada", owner.lastStatusCode_);
    }
    return true;
  }
};

bool GDriveClient::getNoteSyncState(const char *wavPath, NoteSyncState &outState) {
  if (!outState.load(wavPath)) return false;
  String paths[] = {String(wavPath), siblingPath(wavPath, ".txt"), siblingPath(wavPath, ".md")};
  bool current = outState.fullySynced && outState.authGeneration == SettingsStore::currentDriveGeneration();
  for (int i = 0; i < 3; ++i) {
    Asset a = asset(outState, i);
    bool exists = NoteFiles::fs().exists(paths[i].c_str());
    if (!exists) {
      if (a.uploaded) current = false;
      a.uploaded = false;
      if (i == 0) current = false;
      continue;
    }
    size_t size;
    String hash;
    if (!a.uploaded || !isId(a.id) || !isMd5(a.hash) ||
        !fileSnapshot(paths[i].c_str(), i != 0, size, hash) ||
        size != a.size || !hash.equalsIgnoreCase(a.hash)) {
      a.uploaded = false;
      current = false;
    }
  }
  // Visao dos arquivos atuais; nao grava uma consulta de UI. O job persistira
  // a normalizacao antes de qualquer mutacao remota. STT e avaliado por needsUpload.
  outState.fullySynced = current;
  return true;
}

bool GDriveClient::needsUpload(const char *wavPath, bool requireText) {
  NoteSyncState state;
  if (!state.load(wavPath) || !state.fullySynced || !state.folderId[0] ||
      state.authGeneration != SettingsStore::currentDriveGeneration()) return true;
  String paths[] = {String(wavPath), siblingPath(wavPath, ".txt"), siblingPath(wavPath, ".md")};
  for (int i = 0; i < 3; ++i) {
    Asset a = asset(state, i);
    bool exists = NoteFiles::fs().exists(paths[i].c_str());
    if (!exists) {
      if (i == 0 || requireText || a.uploaded) return true;
      continue;
    }
    size_t size;
    String hash;
    if (!fileSnapshot(paths[i].c_str(), i != 0, size, hash) || !a.uploaded ||
        !isId(a.id) || !isMd5(a.hash) || size != a.size ||
        !hash.equalsIgnoreCase(a.hash) || a.offset != size || !a.session.isEmpty()) return true;
  }
  return false;
}

bool GDriveClient::uploadNote(SettingsStore &settingsStore, const char *wavPath,
                               const char *txtPath, const char *mdPath, SyncProgressFn onProgress) {
  SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "preparando nota Drive");
  bindCredentials(settingsStore.get());
  lastError_ = "";
  lastStatusCode_ = 0;
  NoteSyncState state;
  bool loaded = state.load(wavPath);
  if (!loaded && state.lastError[0]) {
    fail(state.lastError, state.lastStatusCode);
    if (onProgress) { SyncProgress p; p.stage = SyncStage::Error; p.message = lastError_.c_str(); onProgress(p); }
    return false;
  }
  Job job(*this, settingsStore, state, wavPath, onProgress);
  if (state.authGeneration != settingsStore.get().driveAuthGeneration) {
    state.fullySynced = false;
    state.pendingFolderId[0] = '\0';
    for (int i = 0; i < 3; ++i) {
      Asset a = asset(state, i);
      a.uploaded = false; a.reserved = false; a.id[0] = '\0'; a.session = ""; a.offset = 0;
    }
    state.authGeneration = settingsStore.get().driveAuthGeneration;
  }
  String paths[] = {String(wavPath), txtPath ? String(txtPath) : siblingPath(wavPath, ".txt"),
                    mdPath ? String(mdPath) : siblingPath(wavPath, ".md")};
  bool present[3] = {};
  bool requireText = settingsStore.get().sttEndpoint[0] != '\0';
  state.fullySynced = false;
  // Validar os assets e gravar o estado ANTES de OAuth/POST/PATCH/upload.
  for (int i = 0; i < 3; ++i) {
    Asset a = asset(state, i);
    job.snapshots[i].path = paths[i];
    present[i] = !paths[i].isEmpty() && NoteFiles::fs().exists(paths[i].c_str());
    if (!present[i]) {
      a.uploaded = false;
      a.session = "";
      a.offset = 0;
      a.hash[0] = '\0';
      a.size = 0;
      continue;
    }
    size_t size;
    String hash;
    if (!fileSnapshot(paths[i].c_str(), i != 0, size, hash, &job.snapshots[i].guard)) {
      a.uploaded = false;
      return job.error(i == 0 ? "WAV local invalido ou ilegivel" : "Texto local vazio, invalido ou ilegivel", 0);
    }
    job.snapshots[i].size = size;
    job.snapshots[i].hash = hash;
    job.snapshots[i].valid = true;
    if (size != a.size || !hash.equalsIgnoreCase(a.hash)) {
      a.uploaded = false;
      a.session = "";
      a.offset = 0;
      a.size = size;
      copyString(a.hash, 33, hash);
    }
    if (!a.id[0]) a.uploaded = false;
  }
  if (!job.persist()) return false;
  if (!present[0]) return job.error("Arquivo WAV local ausente", 0);
  if (requireText && (!present[1] || !present[2])) return job.error("STT configurado: TXT e MD validos sao obrigatorios antes do upload", 0);
  if (!settingsStore.hasDriveAuth()) return job.error("Google Drive ainda nao pareado", 0);
  job.progress(SyncStage::Authenticating, "", 0, 0, 1, "obtendo autorizacao");
  if (!refreshAccessToken(settingsStore.get(), job.token)) {
    // invalid_grant e definitivo: a proxima conexao deve pedir autorizacao,
    // em vez de insistir indefinidamente com o mesmo refresh token invalido.
    if (lastStatusCode_ == 400 && lastError_.startsWith("Autorizacao revogada ou expirada")) {
      if (!settingsStore.saveDriveRefreshToken("")) return job.error("Autorizacao invalida; falha ao invalidar token em NVS", lastStatusCode_);
    }
    return job.error(lastError_, lastStatusCode_);
  }
  if (!job.persist() || !job.ensureFolder()) return false;
  if (job.folder != state.folderId) {
    if (!copyString(state.folderId, sizeof(state.folderId), job.folder)) return job.error("ID da pasta invalido", lastStatusCode_);
    for (int i = 0; i < 3; ++i) {
      Asset a = asset(state, i);
      a.uploaded = false;
      a.session = "";
      a.offset = 0;
    }
    if (!job.persist()) return false;
  }
  const char *mime[] = {"audio/wav", "text/plain", "text/markdown"};
  const SyncStage stages[] = {SyncStage::UploadingWav, SyncStage::UploadingTxt, SyncStage::UploadingMd};
  if (!job.prepareAssets(present)) return false;
  for (int i = 0; i < 3; ++i) {
    if (present[i] && !job.upload(i, paths[i].c_str(), mime[i], stages[i])) return false;
  }
  // Revalidar todos os locais no final: um asset novo/removido/modificado no
  // meio do job nao pode herdar fullySynced de uma fotografia antiga.
  SyncTelemetry::phase(SyncTelemetry::Phase::Verifying, "validando fotografia local final");
  for (int i = 0; i < 3; ++i) {
    bool exists = NoteFiles::fs().exists(paths[i].c_str());
    if (exists != present[i]) {
      asset(state, i).uploaded = false;
      return job.error("Conjunto de arquivos locais mudou durante a sincronizacao", lastStatusCode_);
    }
    if (present[i] && !job.localUnchanged(asset(state, i), paths[i].c_str(), i != 0)) return false;
  }
  state.fullySynced = state.wavUploaded && (!present[1] || state.txtUploaded) &&
                      (!present[2] || state.mdUploaded) &&
                      (!requireText || (present[1] && present[2] && state.txtUploaded && state.mdUploaded));
  if (!state.fullySynced) return job.error("Nota permanece pendente de arquivos verificados", lastStatusCode_);
  lastError_ = "";
  if (!job.persist()) return false;
  String name = baseName(wavPath);
  job.progress(SyncStage::Done, name.c_str(), state.wavBytesUploaded, state.wavSize, 1, "nota sincronizada e verificada");
  return true;
}
