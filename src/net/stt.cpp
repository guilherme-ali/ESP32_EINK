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

// Extrai o valor de "text":"..." ou "content":"..." de um corpo JSON simples - serve
// tanto para {"text": "..."} (OpenAI/Groq STT), {"choices":[{"message":{"content":"..."}}]} (OpenAI/Groq Chat)
// quanto para candidates[0].content.parts[0].text (Gemini). Decodifica escapes \n, \t e \uXXXX (incluindo surrogate pairs).
bool extractJsonText(const String &body, char *out, size_t outLen) {
  int key = body.indexOf("\"text\"");
  if (key < 0) key = body.indexOf("\"content\"");
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
      if (next == 'r') { i += 2; continue; }
      if (next == 't') { out[o++] = '\t'; i += 2; continue; }
      if (next == '"' || next == '\\' || next == '/') { out[o++] = next; i += 2; continue; }
      if (next == 'u' && i + 5 < (int)body.length()) {
        char hex[5] = {body[i + 2], body[i + 3], body[i + 4], body[i + 5], '\0'};
        uint32_t cp = strtoul(hex, nullptr, 16);
        i += 6;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < (int)body.length() &&
            body[i] == '\\' && body[i + 1] == 'u') {
          char hex2[5] = {body[i + 2], body[i + 3], body[i + 4], body[i + 5], '\0'};
          uint32_t cp2 = strtoul(hex2, nullptr, 16);
          if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
            cp = 0x10000 + (((cp & 0x3FF) << 10) | (cp2 & 0x3FF));
            i += 6;
          }
        }
        if (cp < 0x80) {
          out[o++] = (char)cp;
        } else if (cp < 0x800) {
          if (o + 2 < outLen) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
          }
        } else if (cp < 0x10000) {
          if (o + 3 < outLen) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
          }
        } else {
          if (o + 4 < outLen) {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
          }
        }
        continue;
      }
      i++;
      continue;
    }
    out[o++] = c;
    i++;
  }
  out[o] = '\0';
  return o > 0;
}

void escapeJson(const char *src, String &out) {
  while (*src) {
    char c = *src++;
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') continue;
    else if (c == '\t') out += "\\t";
    else out += c;
  }
}

String resolveGeminiModel(const char *configuredModel) {
  String m = String(configuredModel);
  m.trim();
  if (m.startsWith("models/")) m = m.substring(7);
  if (m.length() == 0 || m.indexOf("whisper") >= 0) {
    return "gemini-3.6-flash";
  }
  return m;
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
  String model = resolveGeminiModel(cfg.sttModel);
  String path = "/v1beta/models/" + model + ":generateContent";
  if (cfg.sttApiKey[0] != '\0') {
    path += "?key=" + String(cfg.sttApiKey);
  }

  const char *kPrefix =
      "{\"contents\":[{\"parts\":[{\"text\":\"Transcreva o audio a seguir literalmente, "
      "em portugues do Brasil. Responda so com a transcricao, sem comentarios.\"},"
      "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
  const char *kSuffix = "\"}}]}]}";

  size_t base64Len = 4 * ((fileSize + 2) / 3);
  size_t contentLength = strlen(kPrefix) + base64Len + strlen(kSuffix);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);

  Serial.printf("[STT/Gemini] conectando a %s:%u (modelo %s)...\n", host.c_str(), port, model.c_str());
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

bool generateSummaryGemini(const Settings &cfg, const String &host, uint16_t port,
                            const char *transcriptText, char *outMarkdown, size_t outLen) {
  String model = resolveGeminiModel(cfg.sttModel);
  String path = "/v1beta/models/" + model + ":generateContent";
  if (cfg.sttApiKey[0] != '\0') {
    path += "?key=" + String(cfg.sttApiKey);
  }

  const char *kPrompt =
      "Voce e um assistente pessoal inteligente especializado em sintetizar notas de voz em portugues do Brasil, "
      "no formato Markdown estruturado em topicos.\\n\\n"
      "Analise a transcricao a seguir, identifique o contexto e estruture o documento de forma inteligente conforme o tipo de nota:\\n"
      "1. Se for Ideia/Projeto/Conceito: # 💡 Ideia: [Titulo Curto]\\n## Conceito Principal\\n## Pontos-Chave\\n## Proximos Passos\\n"
      "2. Se for Tarefas/Lembretes: # 📋 Tarefas: [Titulo]\\n## Objetivo\\n## Acoes a Realizar (- [ ] item)\\n## Observacoes ou Prazos\\n"
      "3. Se for Reuniao/Conversa: # 👥 Reuniao: [Tema Principal]\\n## Contexto e Topicos Discutidos\\n## Decisoes Tomadas\\n## Acoes e Responsaveis (- [ ] tarefa)\\n"
      "4. Se for Nota Rapida/Reflexao: # 📝 Nota: [Assunto]\\n## Sintese em Topicos\\n\\n"
      "Diretrizes: Seja conciso, direto e limpo. Responda apenas com o texto Markdown formatado, sem preambulos ou introducoes.\\n\\n"
      "Transcricao:\\n";

  String escapedTranscript;
  escapedTranscript.reserve(strlen(transcriptText) * 2 + 64);
  escapeJson(transcriptText, escapedTranscript);

  const char *kPrefix = "{\"contents\":[{\"parts\":[{\"text\":\"";
  const char *kSuffix = "\"}]}]}";
  size_t contentLength = strlen(kPrefix) + strlen(kPrompt) + escapedTranscript.length() + strlen(kSuffix);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);

  Serial.printf("[Summary/Gemini] conectando a %s:%u (modelo %s)...\n", host.c_str(), port, model.c_str());
  if (!client.connect(host.c_str(), port)) {
    Serial.println("[Summary] falha ao conectar.");
    return false;
  }

  client.printf("POST %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", host.c_str());
  client.printf("x-goog-api-key: %s\r\n", cfg.sttApiKey);
  client.print("Content-Type: application/json\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.print("Connection: close\r\n\r\n");

  client.print(kPrefix);
  client.print(kPrompt);
  client.print(escapedTranscript);
  client.print(kSuffix);

  Serial.println("[Summary] aguardando resposta...");
  String respBody;
  bool ok = readHttpResponse(client, respBody);
  if (!ok) {
    Serial.println("[Summary] resposta HTTP nao-200:");
    Serial.println(respBody);
    return false;
  }
  if (!extractJsonText(respBody, outMarkdown, outLen)) {
    Serial.println("[Summary] nao encontrou texto no retorno:");
    Serial.println(respBody);
    return false;
  }
  return true;
}

bool generateSummaryOpenAi(const Settings &cfg, const String &host, uint16_t port,
                           const String &path, const char *transcriptText,
                           char *outMarkdown, size_t outLen) {
  (void)path;
  String chatPath = "/v1/chat/completions";
  String model = cfg.sttModel;
  if (model.indexOf("whisper") >= 0) {
    if (host.indexOf("groq.com") >= 0) model = "llama-3.3-70b-versatile";
    else model = "gpt-4o-mini";
  }

  const char *kSystem =
      "Você é um assistente pessoal inteligente especializado em sintetizar notas de voz em português do Brasil, "
      "no formato Markdown estruturado em tópicos. Identifique se é ideia, tarefa, reunião ou nota rápida e estruture com tópicos e checkboxes. "
      "Seja direto e conciso, sem preâmbulos.";

  String escapedSys, escapedTranscript;
  escapeJson(kSystem, escapedSys);
  escapeJson(transcriptText, escapedTranscript);

  String body = "{\"model\":\"" + model + "\",\"messages\":["
                "{\"role\":\"system\",\"content\":\"" + escapedSys + "\"},"
                "{\"role\":\"user\",\"content\":\"" + escapedTranscript + "\"}]}";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  Serial.printf("[Summary] conectando a %s:%u...\n", host.c_str(), port);
  if (!client.connect(host.c_str(), port)) {
    Serial.println("[Summary] falha ao conectar.");
    return false;
  }

  client.printf("POST %s HTTP/1.1\r\n", chatPath.c_str());
  client.printf("Host: %s\r\n", host.c_str());
  if (cfg.sttApiKey[0] != '\0') {
    client.printf("Authorization: Bearer %s\r\n", cfg.sttApiKey);
  }
  client.print("Content-Type: application/json\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)body.length());
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  Serial.println("[Summary] aguardando resposta...");
  String respBody;
  bool ok = readHttpResponse(client, respBody);
  if (!ok) {
    Serial.println("[Summary] resposta HTTP nao-200:");
    Serial.println(respBody);
    return false;
  }
  if (!extractJsonText(respBody, outMarkdown, outLen)) {
    Serial.println("[Summary] nao encontrou texto no retorno:");
    Serial.println(respBody);
    return false;
  }
  return true;
}

bool SttClient::generateSummary(const Settings &cfg, const char *transcriptText,
                                char *outMarkdown, size_t outLen) {
  if (!transcriptText || strlen(transcriptText) == 0) return false;
  if (cfg.sttEndpoint[0] == '\0') {
    Serial.println("[Summary] endpoint nao configurado.");
    return false;
  }

  String host, path;
  uint16_t port;
  if (!parseUrl(cfg.sttEndpoint, host, port, path)) {
    Serial.println("[Summary] URL invalida.");
    return false;
  }

  constexpr int kMaxAttempts = 2;
  for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
    bool ok;
    if (host == "generativelanguage.googleapis.com") {
      ok = generateSummaryGemini(cfg, host, port, transcriptText, outMarkdown, outLen);
    } else {
      ok = generateSummaryOpenAi(cfg, host, port, path, transcriptText, outMarkdown, outLen);
    }
    if (ok) {
      Serial.printf("[Summary] resumo gerado (%u chars).\n", (unsigned)strlen(outMarkdown));
      return true;
    }
    if (attempt < kMaxAttempts) {
      Serial.printf("[Summary] tentativa %d falhou, tentando em 2s...\n", attempt);
      delay(2000);
    }
  }

  Serial.println("[Summary] falha ao gerar resumo apos todas as tentativas.");
  return false;
}
