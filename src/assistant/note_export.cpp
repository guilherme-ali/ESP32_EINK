#include "note_export.h"
#include "live_protocol.h"
#include "transport.h"
#include "../storage/note_files.h"
#include <mbedtls/sha256.h>
#include <algorithm>
#include <cstring>
#include <new>

struct NoteExportJob::Context {
  NoteExportJob *owner;
  ConnectFn connect;
  std::string url, token, wav;
  std::atomic<bool> cancelled{false}, launched{false};
  TaskHandle_t handle = nullptr;
  bool ok = false;
  char message[256] = {};
  ~Context() { std::fill(token.begin(), token.end(), '\0'); }
  static bool stop(void *arg) { return static_cast<Context *>(arg)->cancelled.load(); }
  void result(bool success, const char *text) {
    ok = success; strlcpy(message, text, sizeof(message));
  }
  void execute() {
    if (cancelled.load()) { result(false, "Exportacao cancelada."); return; }
    String base(wav.c_str()); base = base.substring(0, base.length() - 4);
    File file = NoteFiles::fs().open(base + ".md", FILE_READ);
    if (!file || !file.size()) { file.close(); file = NoteFiles::fs().open(base + ".txt", FILE_READ); }
    if (!file || !file.size()) { result(false, "Sincronize a nota primeiro para gerar transcricao ou Markdown."); return; }
    // Reserva espaço para frontmatter e título dentro dos 16 KiB do gateway.
    if (file.size() > 15000) { result(false, "Texto acima do limite de exportacao (15.000 bytes). O original foi preservado."); return; }
    const size_t length = file.size();
    std::string content(length, '\0');
    const size_t read = file.read(reinterpret_cast<uint8_t *>(&content[0]), length);
    file.close();
    if (read != length || !Assistant::validUtf8(reinterpret_cast<const uint8_t *>(content.data()), content.size())) {
      result(false, "Falha ao ler texto UTF-8 da nota."); return;
    }
    const size_t slash = wav.find_last_of('/');
    const std::string label = wav.substr(slash == std::string::npos ? 0 : slash + 1,
      wav.size() - (slash == std::string::npos ? 0 : slash + 1) - 4);
    for (char c : label) if (!(c >= '0' && c <= '9') && c != '-') {
      result(false, "Identificador de nota invalido para exportacao."); return;
    }
    if (label.empty()) { result(false, "Identificador de nota vazio."); return; }
    content = "---\ntipo: nota_de_voz\nstatus: processado\ntags: [pessoal/gravador]\ngravador_id: \"" +
      label + "\"\n---\n\n" + content;
    const std::string path = "00_Inbox/Gravador/" + label + ".md";
    Assistant::Json args(cJSON_CreateObject(), cJSON_Delete);
    if (!args || !cJSON_AddStringToObject(args.get(), "path", path.c_str()) ||
        !cJSON_AddStringToObject(args.get(), "content", content.c_str())) {
      result(false, "Memoria insuficiente para exportar a nota."); return;
    }
    const std::string identity = url + "\n" + path + "\n" + content;
    uint8_t digest[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const uint8_t *>(identity.data()), identity.size(), digest, 0)) {
      result(false, "Falha ao calcular identidade da exportacao."); return;
    }
    static const char hex[] = "0123456789abcdef";
    std::string id("note_");
    for (uint8_t byte : digest) { id += hex[byte >> 4]; id += hex[byte & 15]; }
    std::string payload = Assistant::gatewayRequest(token, id, "vault_create", Assistant::printJson(args.get()));
    if (payload.empty()) { result(false, "Pedido de exportacao acima dos limites do gateway."); return; }
    if (cancelled.load() || !connect()) { result(false, "Sem internet para exportar ao Obsidian."); return; }
    std::string response;
    for (unsigned attempt = 0; attempt < 2 && !cancelled.load(); ++attempt) {
      response = Assistant::ToolGateway::execute(url, payload, id, stop, this);
      Assistant::Json root = Assistant::parseJson(response.data(), response.size(), Assistant::kGatewayLimit);
      if (root && Assistant::trueField(root.get(), "ok")) {
        result(true, "Nota criada no GitHub em 00_Inbox/Gravador. O Obsidian recebe a alteracao no proximo pull.");
        std::fill(payload.begin(), payload.end(), '\0'); return;
      }
      const std::string code = root ? Assistant::stringField(Assistant::field(root.get(), "error"), "code") : "";
      if (code != "OUTCOME_UNKNOWN" && code != "REMOTE_UNCERTAIN") break;
      // Uma resposta perdida repete EXATAMENTE a identidade e o corpo original.
    }
    std::fill(payload.begin(), payload.end(), '\0');
    Assistant::Json root = Assistant::parseJson(response.data(), response.size(), Assistant::kGatewayLimit);
    const char *message = root ? Assistant::stringField(Assistant::field(root.get(), "error"), "message") : "";
    const std::string safe = Assistant::redact(message, "", token);
    result(false, cancelled.load() ? "Exportacao cancelada; uma acao ja enviada pode ter sido concluida no GitHub."
                                 : safe.empty() ? "Exportacao nao confirmada; tente novamente para reconciliar." : safe.c_str());
  }
};

bool NoteExportJob::start(const Settings &settings, const char *wavPath, ConnectFn connect) {
  reap();
  if (context_) return false;
  view_ = Snapshot{};
  std::string path;
  if (!wavPath || !String(wavPath).endsWith(".wav") || !connect ||
      !Assistant::gatewayOrigin(settings.assistantGatewayUrl, path) || strlen(settings.assistantGatewayToken) < 32) {
    view_.finished = true;
    strlcpy(view_.message, "Configure o gateway Obsidian/Agenda e seu token no portal.", sizeof(view_.message));
    return false;
  }
  Context *context = new (std::nothrow) Context;
  if (!context) { view_.finished = true; strlcpy(view_.message, "Memoria insuficiente.", sizeof(view_.message)); return false; }
  context_ = context; context->owner = this; context->connect = connect;
  context->url = settings.assistantGatewayUrl; context->token = settings.assistantGatewayToken; context->wav = wavPath;
  if (xTaskCreatePinnedToCore(worker, "obsidian_export", 16384, context, 2, &context->handle, 0) != pdPASS) {
    delete context; context_ = nullptr; view_.finished = true;
    strlcpy(view_.message, "Falha ao criar worker de exportacao.", sizeof(view_.message)); return false;
  }
  context->launched.store(true, std::memory_order_release);
  return true;
}
void NoteExportJob::worker(void *arg) {
  auto *context = static_cast<Context *>(arg);
  while (!context->launched.load(std::memory_order_acquire)) vTaskDelay(1);
  context->execute();
  vTaskSuspend(nullptr);
}
void NoteExportJob::reap() {
  if (!context_ || !context_->handle || eTaskGetState(context_->handle) != eSuspended) return;
  vTaskDelete(context_->handle); context_->handle = nullptr;
  portENTER_CRITICAL(&mux_);
  view_.ok = context_->ok;
  strlcpy(view_.message, context_->message, sizeof(view_.message));
  view_.finished = true;
  portEXIT_CRITICAL(&mux_);
}
void NoteExportJob::cancel() { if (context_) context_->cancelled.store(true); }
bool NoteExportJob::active() const {
  const_cast<NoteExportJob *>(this)->reap();
  return context_ && !view_.finished;
}
bool NoteExportJob::snapshot(Snapshot &out) const {
  const_cast<NoteExportJob *>(this)->reap();
  portENTER_CRITICAL(&mux_); out = view_; portEXIT_CRITICAL(&mux_); return true;
}
bool NoteExportJob::acknowledgeFinished() {
  reap();
  if (!view_.finished) return false;
  delete context_; context_ = nullptr; view_ = Snapshot{}; return true;
}
