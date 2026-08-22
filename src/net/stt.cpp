#include "stt.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <base64.h>

namespace {

// Parser bem simples de URL - so o suficiente para "https://host[:porta]/caminho".
bool parseUrl(const String &url, String &host, uint16_t &port, String &path) {
  if (!url.startsWith("https://")) return false;
  String rest = url.substring(8);
  int slash = rest.indexOf('/');
  String hostPort = slash >= 0 ? rest.substring(0, slash) : rest;
  path = slash >= 0 ? rest.substring(slash) : "/";

  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = hostPort.substring(colon + 1).toInt();
  } else {
    host = hostPort;
    port = 443;
  }
  return host.length() > 0;
}

// Extrai o valor de "text":"..." de um corpo JSON simples - serve tanto
// para {"text": "..."} (OpenAI/Groq) quanto para
// candidates[0].content.parts[0].text (Gemini), ja que os dois usam a
// mesma chave "text" e so precisamos do primeiro valor.
bool extractJsonText(const String &body, char *out, size_t outLen) {
  int key = body.indexOf("\"text\"");
  if (key < 0) return false;
  int colon = body.indexOf(':', key);
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
      char next = body[i + 1];
      if (next == 'n') { out[o++] = '\n'; i += 2; continue; }
      if (next == 't') { out[o++] = '\t'; i += 2; continue; }
      if (next == '"' || next == '\\' || next == '/') { out[o++] = next; i += 2; continue; }
      i++;
      continue;
    }
    out[o++] = c;
    i++;
  }
  out[o] = '\0';
  return o > 0;
}

// Le a resposta HTTP (status + headers + corpo) de um client ja conectado.
bool readHttpResponse(WiFiClientSecure &client, String &body) {
  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 20000) {
    delay(20);
  }
  delay(50); // deixa o resto da resposta chegar - evita ler a status line pela metade

  // Respostas maiores (mais dados chegando via TLS) as vezes fazem a
  // primeira leitura vir vazia por um glitch transitorio do socket;
  // tenta de novo antes de desistir.
  String statusLine = client.readStringUntil('\n');
  for (int attempt = 0; statusLine.length() == 0 && attempt < 5 && client.connected(); attempt++) {
    delay(50);
    statusLine = client.readStringUntil('\n');
  }
  bool ok = statusLine.indexOf(" 200 ") >= 0;

  bool chunked = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.indexOf("Transfer-Encoding") >= 0 && line.indexOf("chunked") >= 0) {
      chunked = true;
    }
    if (line == "\r" || line.length() == 0) break;
  }

  body.reserve(2048);
  if (chunked) {
    // Corpo em pedacos: <tamanho em hex>\r\n<dados>\r\n ... 0\r\n\r\n
    while (client.connected() || client.available()) {
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      long read = 0;
      while (read < chunkSize) {
        if (client.available()) {
          body += (char)client.read();
          read++;
          if (body.length() > 8192) break;
        } else if (!client.connected()) {
          break;
        }
      }
      client.readStringUntil('\n'); // \r\n depois do chunk
      if (body.length() > 8192) break;
    }
  } else {
    while (client.available() || client.connected()) {
      while (client.available()) {
        body += (char)client.read();
        if (body.length() > 8192) break;
      }
      if (!client.connected() && !client.available()) break;
    }
  }
  client.stop();
  return ok;
}

// application/json com o audio em base64 inline (contents[].parts[].inlineData) -
// formato da API do Gemini (generateContent). O endpoint configurado deve
// ser a base "https://generativelanguage.googleapis.com" e o modelo algo
// como "gemini-2.5-flash"; a URL completa e montada aqui.
bool transcribeGemini(const Settings &cfg, const String &host, uint16_t port, File &file,
                       size_t fileSize, char *outText, size_t outLen) {
  String path = "/v1beta/models/" + String(cfg.sttModel) + ":generateContent";

  const char *kPrefix =
      "{\"contents\":[{\"parts\":[{\"text\":\"Transcreva o audio a seguir literalmente, "
      "em portugues do Brasil. Responda so com a transcricao, sem comentarios.\"},"
      "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  const char *kSuffix = "\"}}]}]}";

  size_t base64Len = 4 * ((fileSize + 2) / 3);
  size_t contentLength = strlen(kPrefix) + base64Len + strlen(kSuffix);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  Serial.printf("[STT/Gemini] conectando a %s:%u...\n", host.c_str(), port);
  if (!client.connect(host.c_str(), port)) {
    Serial.println("[STT] falha ao conectar.");
    return false;
  }

  client.printf("POST %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", host.c_str());
  client.printf("x-goog-api-key: %s\r\n", cfg.sttApiKey);
  client.print("Content-Type: application/json\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.print("Connection: close\r\n\r\n");

  client.print(kPrefix);

  // Blocos multiplos de 3 bytes (exceto o ultimo) para que a
  // concatenacao das codificacoes base64 de cada bloco seja identica a
  // codificar o arquivo inteiro de uma vez.
  uint8_t buf[768];
  while (file.available()) {
    size_t n = file.read(buf, sizeof(buf));
    client.print(base64::encode(buf, n));
  }
  client.print(kSuffix);

  Serial.println("[STT] aguardando resposta...");
  String body;
  bool ok = readHttpResponse(client, body);
  if (!ok) {
    Serial.println("[STT] resposta HTTP nao-200:");
    Serial.println(body);
    return false;
  }
  if (!extractJsonText(body, outText, outLen)) {
    Serial.println("[STT] nao encontrei \"text\" na resposta:");
    Serial.println(body);
    return false;
  }
  return true;
}

// multipart/form-data com o WAV bruto - formato compativel com
// OpenAI/Groq (POST .../audio/transcriptions, campos "file" + "model").
bool transcribeOpenAiCompatible(const Settings &cfg, const String &host, uint16_t port,
                                 const String &path, File &file, size_t fileSize,
                                 char *outText, size_t outLen) {
  const String boundary = "----ideiarec7f3b9";
  String partModel = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" +
                      cfg.sttModel + "\r\n";
  String partFileHeader = "--" + boundary +
                           "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n";
  String partFooter = "\r\n--" + boundary + "--\r\n";
  size_t contentLength = partModel.length() + partFileHeader.length() + fileSize + partFooter.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  Serial.printf("[STT] conectando a %s:%u...\n", host.c_str(), port);
  if (!client.connect(host.c_str(), port)) {
    Serial.println("[STT] falha ao conectar.");
    return false;
  }

  client.printf("POST %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", host.c_str());
  if (cfg.sttApiKey[0] != '\0') {
    client.printf("Authorization: Bearer %s\r\n", cfg.sttApiKey);
  }
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.print("Connection: close\r\n\r\n");

  client.print(partModel);
  client.print(partFileHeader);

  uint8_t buf[1024];
  while (file.available()) {
    size_t n = file.read(buf, sizeof(buf));
    client.write(buf, n);
  }
  client.print(partFooter);

  Serial.println("[STT] aguardando resposta...");
  String body;
  bool ok = readHttpResponse(client, body);
  if (!ok) {
    Serial.println("[STT] resposta HTTP nao-200:");
    Serial.println(body);
    return false;
  }
  if (!extractJsonText(body, outText, outLen)) {
    Serial.println("[STT] nao encontrei \"text\" na resposta:");
    Serial.println(body);
    return false;
  }
  return true;
}

} // namespace

bool SttClient::transcribe(const Settings &cfg, const char *wavPath, char *outText, size_t outLen) {
  if (cfg.sttEndpoint[0] == '\0') {
    Serial.println("[STT] endpoint nao configurado.");
    return false;
  }

  String host, path;
  uint16_t port;
  if (!parseUrl(cfg.sttEndpoint, host, port, path)) {
    Serial.println("[STT] URL invalida (precisa comecar com https://).");
    return false;
  }

  // Erros 503/sobrecarga do provedor sao comuns no tier gratuito e
  // costumam sumir sozinhos - vale uma segunda tentativa antes de
  // desistir e deixar a nota sem transcricao.
  constexpr int kMaxAttempts = 2;
  for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
    File file = LittleFS.open(wavPath, FILE_READ);
    if (!file) {
      Serial.println("[STT] nao foi possivel abrir o WAV.");
      return false;
    }
    size_t fileSize = file.size();

    bool ok;
    if (host == "generativelanguage.googleapis.com") {
      ok = transcribeGemini(cfg, host, port, file, fileSize, outText, outLen);
    } else {
      ok = transcribeOpenAiCompatible(cfg, host, port, path, file, fileSize, outText, outLen);
    }
    file.close();

    if (ok) {
      Serial.printf("[STT] transcrito (%u chars).\n", (unsigned)strlen(outText));
      return true;
    }

    if (attempt < kMaxAttempts) {
      Serial.printf("[STT] tentativa %d falhou, tentando de novo em 2s...\n", attempt);
      delay(2000);
    }
  }

  Serial.println("[STT] desistindo apos todas as tentativas.");
  return false;
}
