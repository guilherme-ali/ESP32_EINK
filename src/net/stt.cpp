#include "stt.h"
#include "http_client.h"
#include <FS.h>
#include <string.h>
#include "../audio/wav.h"
#include "../storage/note_files.h"

namespace {
using namespace SttNet;

bool prepare(const Settings &cfg, char *out, size_t outLen,
             String &host, uint16_t &port, String &path, Diagnostics &d) {
  d.reset();
  if (out && outLen) out[0] = '\0';
  if (!out || outLen < 2) {
    d.error = "buffer de saída inválido";
    return false;
  }
  if (!cfg.sttEndpoint[0] || !safeHeader(cfg.sttEndpoint) ||
      !HttpClient::parseHttpsUrl(cfg.sttEndpoint, host, port, path) ||
      host.indexOf('@') >= 0 || path.indexOf('?') >= 0 || path.indexOf('#') >= 0) {
    d.error = "endpoint HTTPS inválido (sem credenciais/query)";
    return false;
  }
  if (!cfg.sttApiKey[0] || !safeHeader(cfg.sttApiKey)) {
    d.error = "chave da API ausente ou inválida";
    return false;
  }
  host.toLowerCase();
  while (host.endsWith(".")) host.remove(host.length() - 1);
  if (!host.length() || (host == "generativelanguage.googleapis.com" && port != 443)) {
    d.error = "host HTTPS inválido ou porta Gemini diferente de 443";
    return false;
  }
  return true;
}

bool wavPathValid(const char *path) {
  if (!path) return false;
  size_t n = strnlen(path, 192);
  if (n < 12 || n == 192 || strncmp(path, "/notes/", 7) != 0 ||
      strcmp(path + n - 4, ".wav") != 0) return false;
  for (size_t i = 7; i < n; ++i) {
    char c = path[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.')) return false;
  }
  return strstr(path, "..") == nullptr;
}

bool wavValid(File &file) {
  WavHeader header;
  if (!file || file.isDirectory() || file.size() <= 44 || !file.seek(0) ||
      file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header)) return false;
  return validWavHeader(header, file.size());
}

String compatibleSttModel(const Settings &cfg, const String &host) {
  String model = normalizedModel(cfg.sttModel);
  // Defaults Gemini não são modelos de transcrição OpenAI/Groq.
  if (!model.length() || model.startsWith("gemini-")) {
    return host == "api.groq.com" ? "whisper-large-v3-turbo" : "whisper-1";
  }
  return model;
}

String compatibleSummaryModel(const Settings &cfg, const String &host) {
  String model = normalizedModel(cfg.summaryModel);
  if (!model.length() || model.startsWith("gemini-") || model.indexOf("whisper") >= 0 ||
      model.indexOf("transcribe") >= 0) {
    return host == "api.groq.com" ? "llama-3.3-70b-versatile" : "gpt-4o-mini";
  }
  return model;
}

String chatPath(const String &endpointPath) {
  int pos = endpointPath.indexOf("/audio/transcriptions");
  if (pos >= 0) return endpointPath.substring(0, pos) + "/chat/completions";
  if (endpointPath.endsWith("/chat/completions")) return endpointPath;
  String base = endpointPath;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!base.length()) base = "/v1";
  return base + "/chat/completions";
}

String transcriptionPath(const String &endpointPath) {
  if (endpointPath.endsWith("/audio/transcriptions")) return endpointPath;
  String base = endpointPath;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!base.length()) base = "/v1";
  return base + "/audio/transcriptions";
}

bool compatibleTranscribe(const Settings &cfg, const String &host, uint16_t port,
                         const String &path, File &wav, char *out, size_t outLen,
                         Diagnostics &d) {
  String model = compatibleSttModel(cfg, host);
  if (!safeModel(model)) { d.error = "modelo STT inválido"; return false; }
  const String boundary = "----eink-stt-" + String((uint32_t)random(0x7fffffff), HEX);
  String prefix = "--" + boundary +
      "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + model +
      "\r\n--" + boundary +
      "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\npt" +
      "\r\n--" + boundary +
      "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson" +
      "\r\n--" + boundary +
      "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\n"
      "Content-Type: audio/wav\r\n\r\n";
  String suffix = "\r\n--" + boundary + "--\r\n";
  size_t length = prefix.length() + wav.size() + suffix.length();
  String contentType = "multipart/form-data; boundary=" + boundary;
  for (unsigned attempt = 0; attempt <= kMaxRetries; ++attempt) {
    Response r;
    d.model = model;
    setPhase(SyncTelemetry::Phase::GeminiUpload, "áudio");
    WiFiClientSecure &client = sharedClient();
    bool ok = beginRequest(client, host, port, "POST", transcriptionPath(path),
                            cfg.sttApiKey, false, contentType.c_str(), length, "", r);
    if (ok) {
      bool sent = write(client, prefix) && writeFile(client, wav) && write(client, suffix);
      if (sent) setPhase(SyncTelemetry::Phase::Transcribing);
      ok = finishRequest(client, r, sent);
    }
    if (ok && r.status == 200 && extractCompatible(r, false, out, outLen, d)) {
      return true;
    }
    if (r.status != 200 || !ok) recordFailure(r, d);
    if (!retryable(r) || attempt == kMaxRetries) return false;
    backoff(attempt, retryHint(r), r.status == 429);
  }
  return false;
}

bool compatibleSummary(const Settings &cfg, const String &host, uint16_t port,
                       const String &path, const char *transcript, char *out,
                       size_t outLen, Diagnostics &d) {
  String model = compatibleSummaryModel(cfg, host);
  if (!safeModel(model)) { d.error = "modelo de resumo inválido"; return false; }
  String qm, qp, qt;
  if (!quote(model.c_str(), qm) || !quote(kSummaryPrompt, qp) || !quote(transcript, qt)) {
    d.error = "não foi possível codificar o resumo em JSON"; return false;
  }
  String body;
  if (!join(body, {"{\"model\":", qm.c_str(), ",\"messages\":[{\"role\":\"system\",\"content\":",
                  qp.c_str(), "},{\"role\":\"user\",\"content\":", qt.c_str(),
                  "}],\"max_tokens\":4096,\"stream\":false}"})) {
    d.error = "memória insuficiente ou " + bodyLimitError("pedido JSON"); return false;
  }
  for (unsigned attempt = 0; attempt <= kMaxRetries; ++attempt) {
    Response r;
    d.model = model;
    setPhase(SyncTelemetry::Phase::Markdown);
    bool ok = requestJson(host, port, "POST", chatPath(path), cfg.sttApiKey, false, body, r,
                          "", kReadTimeoutMs, SyncTelemetry::Phase::Markdown);
    if (ok && r.status == 200 && extractCompatible(r, true, out, outLen, d)) return true;
    if (r.status != 200 || !ok) recordFailure(r, d);
    if (!retryable(r) || attempt == kMaxRetries) return false;
    backoff(attempt, retryHint(r), r.status == 429);
  }
  return false;
}
} // namespace

bool SttClient::transcribe(const Settings &cfg, const char *wavPath, char *outText, size_t outLen) {
  setPhase(SyncTelemetry::Phase::Preparing);
  String host, path;
  uint16_t port = 443;
  if (!prepare(cfg, outText, outLen, host, port, path, diagnostics_)) return false;
  if (!wavPathValid(wavPath)) { diagnostics_.error = "caminho WAV fora de /notes/"; return false; }
  File wav = NoteFiles::openRead(wavPath);
  if (!wavValid(wav)) { diagnostics_.error = "WAV ausente, vazio ou inválido"; return false; }
  bool ok;
  if (host == "generativelanguage.googleapis.com" && port == 443) {
    GeminiClient gemini;
    ok = gemini.transcribe(cfg, wav, outText, outLen, diagnostics_);
  } else {
    ok = compatibleTranscribe(cfg, host, port, path, wav, outText, outLen, diagnostics_);
  }
  wav.close();
  if (!ok) outText[0] = '\0';
  return ok;
}

bool SttClient::generateSummary(const Settings &cfg, const char *transcriptText,
                                char *outMarkdown, size_t outLen) {
  setPhase(SyncTelemetry::Phase::Preparing);
  String host, path;
  uint16_t port = 443;
  if (!prepare(cfg, outMarkdown, outLen, host, port, path, diagnostics_)) return false;
  size_t length = transcriptText ? strnlen(transcriptText, kMaxTextLen) : 0;
  if (!length || length >= kMaxTextLen || !validUtf8(transcriptText, length) ||
      !hasText(transcriptText)) {
    diagnostics_.error = "transcrição vazia, UTF-8 inválido ou acima de " + String(kMaxTextLen - 1) + " bytes";
    return false;
  }
  bool ok;
  if (host == "generativelanguage.googleapis.com" && port == 443) {
    GeminiClient gemini;
    ok = gemini.generateSummary(cfg, transcriptText, outMarkdown, outLen, diagnostics_);
  } else {
    ok = compatibleSummary(cfg, host, port, path, transcriptText, outMarkdown, outLen, diagnostics_);
  }
  if (!ok) outMarkdown[0] = '\0';
  return ok;
}

void SttClient::cleanupPending(const Settings &cfg) {
  String host, path;
  uint16_t port = 443;
  if (HttpClient::parseHttpsUrl(cfg.sttEndpoint, host, port, path)) {
    host.toLowerCase();
    while (host.endsWith(".")) host.remove(host.length() - 1);
    if (host == "generativelanguage.googleapis.com" && port == 443) GeminiClient::cleanupPending(cfg);
  }
}

void SttClient::beginBatch() { GeminiClient::beginBatch(); }
void SttClient::endSession() { GeminiClient::endSession(); }
