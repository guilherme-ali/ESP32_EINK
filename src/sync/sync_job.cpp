#include "sync_job.h"
#include "../audio/wav.h"
#include "../storage/note_files.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <type_traits>

#if defined(CONFIG_FREERTOS_PLACE_TASK_STACKS_IN_EXT_RAM) && CONFIG_FREERTOS_PLACE_TASK_STACKS_IN_EXT_RAM
#error "SyncJob requer a pilha de xTaskCreatePinnedToCore em RAM interna"
#endif

namespace {
constexpr size_t kCopyBytes = 192;
constexpr uint32_t kDefaultRate = 200 * 1024;
constexpr uint32_t kDeadlineMs = 10 * 60 * 1000UL;
static_assert(sizeof(SyncView::NoteProgress) <= kCopyBytes, "Progresso excede copia limitada");
static_assert(std::is_trivially_copyable<SyncJob::Snapshot>::value, "Snapshot deve ser POD");

template <size_t N> void copy(char (&dst)[N], const char *src) {
  if (!src) src = "";
  size_t length = strnlen(src, N - 1);
  // Nao cortar uma sequencia UTF-8 no meio (.perf tambem valida UTF-8).
  if (length == N - 1)
    while (length && (static_cast<uint8_t>(src[length]) & 0xc0) == 0x80) --length;
  memcpy(dst, src, length); dst[length] = '\0';
}
String sibling(const char *wav, const char *extension) {
  String path(wav); path.replace(".wav", extension); return path;
}
uint32_t bounded(uint64_t n, uint32_t low, uint32_t high) {
  return static_cast<uint32_t>(std::min<uint64_t>(high, std::max<uint64_t>(low, n)));
}
uint32_t textSize(const String &path, uint32_t *chars = nullptr) {
  File file = NoteFiles::fs().open(path, FILE_READ);
  uint32_t size = file ? file.size() : 0;
  if (chars) {
    *chars = 0;
    uint8_t buffer[256];
    while (file && file.available()) {
      size_t got = file.read(buffer, sizeof(buffer));
      if (!got) break;
      for (size_t i = 0; i < got; ++i) if ((buffer[i] & 0xc0) != 0x80) ++*chars;
    }
  }
  file.close(); return size;
}
uint32_t textChars(const char *text) {
  uint32_t count = 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
    if ((*p & 0xc0) != 0x80) ++count;
  return count;
}

// Nao inventa modelos para textos legados. Preserva os modelos registrados
// antes do commit e permite reparar apenas o estado auxiliar de textos validos.
bool recordAi(const char *wav, const char *stage, const char *model,
              const char *error, int status) {
  String path = sibling(wav, ".ai");
  cJSON *root = nullptr;
  File file = NoteFiles::fs().open(path, FILE_READ);
  if (file && file.size() < 8192) {
    String saved;
    if (!NoteFiles::readBounded(file, saved, 8191)) { file.close(); return false; }
    root = cJSON_Parse(saved.c_str());
  }
  file.close();
  if (root && !cJSON_IsObject(root)) { cJSON_Delete(root); root = nullptr; }
  if (!root) root = cJSON_CreateObject();
  if (!root) return false;
  const char *key = strcmp(stage, "transcricao") == 0 ? "transcriptModel" : "summaryModel";
  if (model && model[0]) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, model);
  }
  cJSON_DeleteItemFromObjectCaseSensitive(root, "lastError");
  cJSON_DeleteItemFromObjectCaseSensitive(root, "lastStatusCode");
  cJSON_DeleteItemFromObjectCaseSensitive(root, "pendingStage");
  cJSON_AddStringToObject(root, "lastError", error);
  cJSON_AddNumberToObject(root, "lastStatusCode", status);
  cJSON_AddStringToObject(root, "pendingStage", error[0] ? stage : "");
  cJSON *savedModel = cJSON_GetObjectItemCaseSensitive(root, key);
  if ((model && model[0] && (!cJSON_IsString(savedModel) || strcmp(savedModel->valuestring, model))) ||
      !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "lastError")) ||
      !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "pendingStage")) ||
      !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "lastStatusCode"))) {
    cJSON_Delete(root); return false;
  }
  char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
  if (!json) return false;
  bool ok = NoteFiles::writeAtomic(path, json, strlen(json)); free(json); return ok;
}
bool metadataReady(const char *wav, SyncJob::Snapshot &snapshot) {
  String path = sibling(wav, ".ai"); NoteFiles::recoverText(path);
  File file = NoteFiles::fs().open(path, FILE_READ);
  if (!file || file.size() > 8192) { file.close(); return false; }
  String saved;
  if (!NoteFiles::readBounded(file, saved, 8192)) { file.close(); return false; }
  file.close();
  cJSON *root = cJSON_Parse(saved.c_str());
  if (!root) return false;
  cJSON *transcript = cJSON_GetObjectItemCaseSensitive(root, "transcriptModel");
  cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summaryModel");
  if (cJSON_IsString(transcript)) copy(snapshot.transcriptModel, transcript->valuestring);
  if (cJSON_IsString(summary)) copy(snapshot.markdownModel, summary->valuestring);
  cJSON *stage = cJSON_GetObjectItemCaseSensitive(root, "pendingStage");
  cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "lastError");
  bool ready = cJSON_IsString(stage) && !stage->valuestring[0] &&
               cJSON_IsString(error) && !error->valuestring[0];
  cJSON_Delete(root); return ready;
}

// Unsigned NVS, EMA alpha=0.25. Perfis de inferencia separados por modelo;
// taxas de transporte globais, amostras invalidas/erros nao treinam o perfil.
struct History {
  Preferences nvs;
  bool open = false;
  void begin() { open = nvs.begin("syncPerf", false); }
  ~History() { if (open) nvs.end(); }
  uint32_t get(const char *key, uint32_t fallback, uint32_t low, uint32_t high) {
    return bounded(open ? nvs.getUInt(key, fallback) : fallback, low, high);
  }
  void update(const char *key, uint64_t value, uint32_t fallback, uint32_t low, uint32_t high) {
    if (!open || !value) return;
    uint32_t sample = bounded(value, low, high), old = get(key, fallback, low, high);
    if (!nvs.putUInt(key, (static_cast<uint64_t>(old) * 3 + sample + 2) / 4)) openWarning = true;
  }
  bool openWarning = false;
  static uint32_t modelHash(const char *model) {
    uint32_t hash = 2166136261UL;
    for (const char *p = model; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619UL;
    return hash;
  }
  void modelKey(char *out, char prefix, const char *model, bool automatic = false) {
    uint32_t hash = modelHash(model);
    if (automatic && open) hash = nvs.getUInt(prefix == 's' ? "autoSTT" : "autoMD", hash);
    snprintf(out, 16, "%c%08lx", prefix, static_cast<unsigned long>(hash));
  }
  SyncView::Prediction predict(const Settings &cfg, uint32_t audioMs, uint32_t wavBytes,
                               uint32_t chars, uint64_t driveBytes) {
    SyncView::Prediction p;
    char key[16]; modelKey(key, 's', cfg.sttModel, cfg.sttAutoModel);
    p.transcribeMs = bounded(10000ULL + static_cast<uint64_t>(audioMs) * get(key, 750, 10, 60000) / 1000, 10000, 3600000);
    modelKey(key, 'm', cfg.summaryModel, cfg.sttAutoModel);
    p.markdownMs = bounded(8000ULL + static_cast<uint64_t>(chars) * get(key, 1000, 10, 60000) / 1000, 8000, 3600000);
    p.geminiUploadMs = bounded(2000ULL + static_cast<uint64_t>(wavBytes) * 4 / 3 * 1000 /
                              get("gemBps", kDefaultRate, 1024, 20 * 1024 * 1024), 2000, 3600000);
    p.driveUploadMs = bounded(2000ULL + driveBytes * 1000 /
                             get("driveBps", kDefaultRate, 1024, 20 * 1024 * 1024), 2000, 3600000);
    return p;
  }
  void learn(const SyncTelemetry::Counters &c, uint32_t audioMs, uint32_t chars,
             bool hadTranscript, bool hadMarkdown, const SyncJob::Snapshot &snapshot) {
    if (!open) return;
    if (!hadTranscript && snapshot.transcriptModel[0] &&
        !nvs.putUInt("autoSTT", modelHash(snapshot.transcriptModel))) openWarning = true;
    if (!hadMarkdown && snapshot.markdownModel[0] &&
        !nvs.putUInt("autoMD", modelHash(snapshot.markdownModel))) openWarning = true;
    if (c.geminiUploadMs && c.geminiBytes)
      update("gemBps", c.geminiBytes * 1000 / c.geminiUploadMs, kDefaultRate, 1024, 20 * 1024 * 1024);
    if (c.driveUploadMs && c.driveBytes)
      update("driveBps", c.driveBytes * 1000 / c.driveUploadMs, kDefaultRate, 1024, 20 * 1024 * 1024);
    char key[16];
    if (!hadTranscript && snapshot.transcriptModel[0] && audioMs && c.transcribeMs > 10000) {
      modelKey(key, 's', snapshot.transcriptModel);
      update(key, static_cast<uint64_t>(c.transcribeMs - 10000) * 1000 / audioMs, 750, 10, 60000);
    }
    if (!hadMarkdown && snapshot.markdownModel[0] && chars && c.markdownMs > 8000) {
      modelKey(key, 'm', snapshot.markdownModel);
      update(key, static_cast<uint64_t>(c.markdownMs - 8000) * 1000 / chars, 1000, 10, 60000);
    }
    if (!nvs.putUInt("success", get("success", 0, 0, UINT32_MAX - 1) + 1)) openWarning = true;
    uint64_t http = static_cast<uint64_t>(get("http", 0, 0, UINT32_MAX)) + c.httpRequests;
    uint64_t tls = static_cast<uint64_t>(get("tls", 0, 0, UINT32_MAX)) + c.tlsHandshakes;
    if (!nvs.putUInt("http", bounded(http, 0, UINT32_MAX)) ||
        !nvs.putUInt("tls", bounded(tls, 0, UINT32_MAX))) openWarning = true;
  }
};
} // namespace

struct SyncJob::Context {
  struct Entry {
    char wavPath[48] = "", title[24] = "";
    uint32_t wavBytes = 0, audioMs = 0, sampleRate = 0, textChars = 0;
    uint64_t sizes[3] = {}, confirmed[3] = {};
    bool hadTranscript = false, hadMarkdown = false;
    bool initialTranscript = false, initialMarkdown = false;
    bool needsAI = false, needsUpload = false, forceVerify = false;
    bool wavValid = false;
    char error[512] = "";
    SyncTelemetry::Counters counters;
    uint32_t totalMs = 0;
    bool success = false;
  };
  const Settings cfg; // copia imutavel em PSRAM depois da conexao/pareamento
  Entry entries[kMaxNotes];
  unsigned count = 0;
  bool hasStt = false, hasDrive = false;
  char *text = nullptr, *markdown = nullptr;
  explicit Context(const Settings &settings) : cfg(settings) {}
  ~Context() { free(text); free(markdown); }
};

SyncJob *SyncJob::observerOwner_ = nullptr;
SyncJob::SyncJob(SettingsStore &settings, SttClient &stt, GDriveClient &drive)
    : settings_(settings), stt_(stt), drive_(drive) {}

bool SyncJob::isActive() const {
  portENTER_CRITICAL(&mux_); bool value = active_; portEXIT_CRITICAL(&mux_); return value;
}
bool SyncJob::pauseRequested() const {
  portENTER_CRITICAL(&mux_); bool value = pause_; portEXIT_CRITICAL(&mux_); return value;
}
void SyncJob::requestPause() {
  portENTER_CRITICAL(&mux_); if (active_) pause_ = true; portEXIT_CRITICAL(&mux_);
}

// Cada regiao critica copia <=192 bytes, sem FS/NVS/Serial/alocacao. A revisao
// torna coerentes os blocos (incluindo NoteProgress e o erro de 512 bytes).
void SyncJob::publish() {
  portENTER_CRITICAL(&mux_); ++revision_; portEXIT_CRITICAL(&mux_);
  const uint8_t *src = reinterpret_cast<const uint8_t *>(&work_);
  uint8_t *dst = reinterpret_cast<uint8_t *>(&shared_);
  for (size_t i = 0; i < sizeof(Snapshot); i += kCopyBytes) {
    size_t n = std::min(kCopyBytes, sizeof(Snapshot) - i);
    portENTER_CRITICAL(&mux_); memcpy(dst + i, src + i, n); portEXIT_CRITICAL(&mux_);
  }
  portENTER_CRITICAL(&mux_); ++revision_; portEXIT_CRITICAL(&mux_);
}
bool SyncJob::snapshot(Snapshot &out) const {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    portENTER_CRITICAL(&mux_); uint32_t before = revision_; portEXIT_CRITICAL(&mux_);
    if (before & 1) continue;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(&shared_);
    uint8_t *dst = reinterpret_cast<uint8_t *>(&out);
    for (size_t i = 0; i < sizeof(Snapshot); i += kCopyBytes) {
      size_t n = std::min(kCopyBytes, sizeof(Snapshot) - i);
      portENTER_CRITICAL(&mux_); memcpy(dst + i, src + i, n); portEXIT_CRITICAL(&mux_);
    }
    portENTER_CRITICAL(&mux_);
    bool stable = before == revision_ && !(revision_ & 1);
    out.pauseRequested = pause_;
    portEXIT_CRITICAL(&mux_);
    if (stable) return true;
  }
  return false;
}
bool SyncJob::acknowledgeFinished() {
  portENTER_CRITICAL(&mux_);
  bool finished = active_ && !(revision_ & 1) && shared_.finished;
  if (finished) active_ = false;
  portEXIT_CRITICAL(&mux_); return finished;
}

SyncJob::StartResult SyncJob::start(NotesStore &notes, int onlyIndex, const char *onlyPath) {
  if (isActive()) return StartResult::TaskError;
  void *memory = heap_caps_malloc(sizeof(Context), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) return StartResult::NoMemory;
  Context *ctx = new (memory) Context(settings_.get());
  ctx->hasStt = ctx->cfg.sttEndpoint[0] != '\0';
  ctx->hasDrive = ctx->cfg.driveRefreshToken[0] != '\0';
  int count = notes.count(); // cache validado; nunca rehash por nota concluida
  for (int i = 0; i < count && ctx->count < kMaxNotes; ++i) {
    if (onlyIndex >= 0 && i != onlyIndex) continue;
    NoteEntry e;
    if (!notes.getAt(i, e) || (onlyPath && strcmp(e.path, onlyPath))) continue;
    bool ai = ctx->hasStt && (!e.hasTxt || !e.hasMd);
    if (!ai && ctx->hasStt && !ctx->hasDrive) {
      Snapshot metadata;
      ai = !metadataReady(e.path, metadata);
    }
    bool upload = ctx->hasDrive && (!e.hasSnc || ai);
    bool force = onlyIndex >= 0; // "esta nota" revalida explicitamente o Drive
    if (!ai && !upload && !(force && (ctx->hasDrive || ctx->hasStt))) continue;
    Context::Entry &entry = ctx->entries[ctx->count];
    copy(entry.wavPath, e.path); copy(entry.title, e.label);
    entry.hadTranscript = e.hasTxt; entry.hadMarkdown = e.hasMd;
    entry.initialTranscript = e.hasTxt; entry.initialMarkdown = e.hasMd;
    entry.needsAI = ctx->hasStt; // inclui reparo .ai somente na selecao pendente
    entry.needsUpload = upload; entry.forceVerify = force;
    File file = NoteFiles::fs().open(e.path, FILE_READ);
    WavHeader header;
    size_t size = file ? file.size() : 0;
    bool valid = file && file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
                 validWavHeader(header, size);
    file.close();
    entry.wavValid = valid;
    entry.wavBytes = size; entry.sampleRate = valid ? header.sampleRate : 0;
    entry.audioMs = valid ? bounded(static_cast<uint64_t>(header.dataSize) * 1000 / header.byteRate, 0, UINT32_MAX) : 0;
    entry.sizes[0] = size;
    ++ctx->count;
  }
  if (!ctx->count) { ctx->~Context(); free(ctx); return StartResult::NothingPending; }
  context_ = ctx;
  work_ = Snapshot(); work_.totalNotes = ctx->count; work_.startedAt = millis();
  work_.currentNote = 1; copy(work_.noteLabel, ctx->entries[0].title);
  copy(work_.detail, "preparando lote");
  portENTER_CRITICAL(&mux_); pause_ = false; active_ = true; portEXIT_CRITICAL(&mux_);
  publish();
  // No SDK Arduino/IDF deste projeto a pilha e TCB dinamicos sao internos.
  if (xTaskCreatePinnedToCore(taskEntry, "syncJob", 24576, this, 1, nullptr, 0) != pdPASS) {
    context_ = nullptr; ctx->~Context(); free(ctx);
    portENTER_CRITICAL(&mux_); active_ = false; portEXIT_CRITICAL(&mux_);
    return StartResult::TaskError;
  }
  return StartResult::Started;
}

const char *SyncJob::phaseLabel(SyncTelemetry::Phase phase) {
  using P = SyncTelemetry::Phase;
  switch (phase) {
    case P::Preparing: return "preparando";
    case P::GeminiUpload: return "enviando audio IA";
    case P::Transcribing: return "transcrevendo";
    case P::Markdown: return "gerando Markdown";
    case P::DriveUpload: return "enviando Drive";
    case P::Verifying: return "verificando";
    case P::QuotaWait: return "aguardando cota";
    case P::RetryWait: return "nova tentativa";
    case P::Done: return "concluido";
    case P::Error: return "falha";
  }
  return "preparando";
}
void SyncJob::observe(const SyncTelemetry::Update &event) {
  SyncJob *job = observerOwner_;
  if (!job) return;
  job->work_.phase = event.phase;
  copy(job->work_.detail, event.detail); // detail e efemero; nunca guardar ponteiro
  // Eventos de fase (0/0) preservam os offsets ja confirmados pelo modelo.
  job->work_.progress.update(event.phase, event.current, event.total, millis(), event.detail);
  // "Done" de uma API nao equivale a nota entregue; verified() e do controlador.
  job->work_.counters = SyncTelemetry::counters();
  job->publish();
}
bool SyncJob::boundary() {
  if (pauseRequested()) {
    work_.paused = true; copy(work_.detail, "pausa apos etapa"); publish(); return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    copy(work_.error, "Wi-Fi desconectado; sincronize novamente para retomar"); return false;
  }
  return true;
}

void SyncJob::updateDriveTotals(unsigned index) {
  Context::Entry &entry = context_->entries[index];
  entry.sizes[1] = entry.hadTranscript ? textSize(sibling(entry.wavPath, ".txt")) : 0;
  entry.sizes[2] = entry.hadMarkdown ? textSize(sibling(entry.wavPath, ".md")) : 0;
  work_.progress.setDriveTotals(entry.sizes, entry.confirmed);
  publish();
}

bool SyncJob::prepareAi(unsigned index) {
  Context &ctx = *context_; Context::Entry &entry = ctx.entries[index];
  bool ready = metadataReady(entry.wavPath, work_);
  if (!entry.needsAI) return true;
  String txt = sibling(entry.wavPath, ".txt"), md = sibling(entry.wavPath, ".md");
  if (!entry.hadTranscript || !entry.hadMarkdown) {
    if (!ctx.text) ctx.text = static_cast<char *>(heap_caps_malloc(SttClient::kMaxTextLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entry.hadMarkdown && !ctx.markdown) ctx.markdown = static_cast<char *>(heap_caps_malloc(SttClient::kMaxTextLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ctx.text || (!entry.hadMarkdown && !ctx.markdown)) { copy(work_.error, "PSRAM insuficiente para texto"); return false; }
    if (entry.hadTranscript && !NoteFiles::readText(txt, ctx.text, SttClient::kMaxTextLen)) {
      copy(work_.error, "Falha ao ler transcricao validada"); return false;
    }
  }
  if (!entry.hadTranscript) {
    if (!boundary()) return false;
    SyncTelemetry::phase(SyncTelemetry::Phase::GeminiUpload, entry.wavPath);
    if (!stt_.transcribe(ctx.cfg, entry.wavPath, ctx.text, SttClient::kMaxTextLen)) {
      copy(work_.error, (String("Transcricao: ") + stt_.lastError()).c_str());
      recordAi(entry.wavPath, "transcricao", stt_.lastModel().c_str(), stt_.lastError().c_str(), stt_.lastStatusCode());
      return false;
    }
    copy(work_.transcriptModel, stt_.lastModel().c_str());
    SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "salvando transcricao");
    if (!recordAi(entry.wavPath, "transcricao", work_.transcriptModel, "gravacao do TXT pendente", stt_.lastStatusCode())) {
      copy(work_.error, "Falha ao salvar modelo/estado antes do TXT"); return false;
    }
    if (!NoteFiles::writeAtomic(txt, ctx.text, strlen(ctx.text))) {
      copy(work_.error, "Nao foi possivel salvar a transcricao completa"); return false;
    }
    entry.hadTranscript = true;
    entry.textChars = textChars(ctx.text);
    if (!recordAi(entry.wavPath, "transcricao", work_.transcriptModel, "", stt_.lastStatusCode())) {
      copy(work_.error, "TXT salvo; diagnostico ainda pendente"); return false;
    }
    work_.progress.transcriptDone(); updateDriveTotals(index); ready = false;
  }
  if (!entry.hadMarkdown) {
    if (!boundary()) return false;
    entry.textChars = textChars(ctx.text);
    SyncTelemetry::phase(SyncTelemetry::Phase::Markdown, md.c_str());
    if (!stt_.generateSummary(ctx.cfg, ctx.text, ctx.markdown, SttClient::kMaxTextLen)) {
      copy(work_.error, (String("Markdown: ") + stt_.lastError()).c_str());
      recordAi(entry.wavPath, "resumo", stt_.lastModel().c_str(), stt_.lastError().c_str(), stt_.lastStatusCode());
      return false;
    }
    copy(work_.markdownModel, stt_.lastModel().c_str());
    SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, "salvando Markdown");
    if (!recordAi(entry.wavPath, "resumo", work_.markdownModel, "gravacao do Markdown pendente", stt_.lastStatusCode())) {
      copy(work_.error, "Falha ao salvar modelo/estado antes do Markdown"); return false;
    }
    if (!NoteFiles::writeAtomic(md, ctx.markdown, strlen(ctx.markdown))) {
      copy(work_.error, "Nao foi possivel salvar o Markdown completo"); return false;
    }
    entry.hadMarkdown = true;
    if (!recordAi(entry.wavPath, "resumo", work_.markdownModel, "", stt_.lastStatusCode())) {
      copy(work_.error, "Markdown salvo; diagnostico ainda pendente"); return false;
    }
    work_.progress.markdownDone(); updateDriveTotals(index); ready = false;
  }
  if (!ready && !recordAi(entry.wavPath, "resumo", "", "", 200)) {
    copy(work_.error, "Textos salvos; diagnostico ainda pendente"); return false;
  }
  if (!work_.transcriptModel[0] || !work_.markdownModel[0])
    copy(work_.warning, "Modelo de texto legado desconhecido");
  return NoteFiles::validText(txt) && NoteFiles::validText(md) && metadataReady(entry.wavPath, work_);
}

bool SyncJob::processNote(unsigned index) {
  Context &ctx = *context_; Context::Entry &entry = ctx.entries[index];
  if (!entry.wavValid) {
    copy(work_.error, "WAV invalido; arquivo preservado e envio retido"); return false;
  }
  String txt = sibling(entry.wavPath, ".txt"), md = sibling(entry.wavPath, ".md");
  if (!boundary()) return false;
  if (!prepareAi(index)) {
    if (!work_.paused && !work_.error[0]) copy(work_.error, "Textos/diagnostico nao validados");
    return false;
  }
  if (entry.needsAI && (!entry.initialTranscript || !entry.initialMarkdown)) ++work_.ai;
  if (ctx.hasDrive && (entry.needsUpload || entry.forceVerify)) {
    if (!boundary()) return false;
    updateDriveTotals(index);
    if (!drive_.uploadNote(settings_, entry.wavPath,
                          entry.hadTranscript ? txt.c_str() : nullptr,
                          entry.hadMarkdown ? md.c_str() : nullptr)) {
      copy(work_.error, (String("Drive: ") + drive_.lastError()).c_str()); return false;
    }
    // uploadNote retorna true somente apos checksums e estado atomico final.
    ++work_.uploaded; work_.progress.driveDone();
  }
  SyncTelemetry::phase(SyncTelemetry::Phase::Verifying, "validando entrega local");
  if (ctx.hasStt && (!NoteFiles::validText(txt) || !NoteFiles::validText(md) || !metadataReady(entry.wavPath, work_))) {
    copy(work_.error, "Textos/diagnostico final nao validados"); return false;
  }
  work_.progress.verified(); work_.delivered = true;
  return true;
}

void SyncJob::saveReport(unsigned index, bool success, uint32_t startedAt) {
  Context::Entry &entry = context_->entries[index];
  entry.counters = work_.counters; entry.totalMs = millis() - startedAt;
  entry.success = success; copy(entry.error, work_.error);
  const auto &c = entry.counters;
  Serial.printf("[Perf] nota=%s total_ms=%lu preparing_ms=%lu gemini_upload_ms=%lu transcribe_ms=%lu md_ms=%lu drive_ms=%lu verify_ms=%lu quota_ms=%lu retry_ms=%lu HTTP=%lu TLS=%lu\n",
    entry.title, (unsigned long)entry.totalMs, (unsigned long)c.preparingMs,
    (unsigned long)c.geminiUploadMs, (unsigned long)c.transcribeMs, (unsigned long)c.markdownMs,
    (unsigned long)c.driveUploadMs, (unsigned long)c.verifyingMs,
    (unsigned long)c.quotaWaitMs, (unsigned long)c.retryWaitMs,
    (unsigned long)c.httpRequests, (unsigned long)c.tlsHandshakes);
  if (entry.error[0]) Serial.printf("[SyncError] nota=%s erro=%s\n", entry.title, entry.error);
  cJSON *root = cJSON_CreateObject();
  if (!root) { copy(work_.warning, "Sem memoria para .perf"); return; }
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON_AddStringToObject(root, "note", entry.title);
  cJSON_AddBoolToObject(root, "success", success);
  cJSON_AddBoolToObject(root, "hadTranscription", entry.initialTranscript);
  cJSON_AddBoolToObject(root, "hadMarkdown", entry.initialMarkdown);
  cJSON_AddNumberToObject(root, "startedAtMs", startedAt);
  cJSON_AddStringToObject(root, "transcriptModel", work_.transcriptModel);
  cJSON_AddStringToObject(root, "summaryModel", work_.markdownModel);
  cJSON_AddStringToObject(root, "error", entry.error);
  cJSON_AddStringToObject(root, "warning", work_.warning);
  cJSON_AddNumberToObject(root, "totalMs", entry.totalMs);
  cJSON_AddNumberToObject(root, "audioMs", entry.audioMs);
  cJSON_AddNumberToObject(root, "sampleRateHz", entry.sampleRate);
  cJSON_AddNumberToObject(root, "wavBytes", entry.wavBytes);
  cJSON_AddNumberToObject(root, "textChars", entry.textChars);
  cJSON_AddNumberToObject(root, "httpRequests", c.httpRequests);
  cJSON_AddNumberToObject(root, "tlsHandshakes", c.tlsHandshakes);
  cJSON_AddNumberToObject(root, "geminiBytes", c.geminiBytes);
  cJSON_AddNumberToObject(root, "driveBytes", c.driveBytes);
  cJSON_AddNumberToObject(root, "preparingMs", c.preparingMs);
  cJSON_AddNumberToObject(root, "geminiUploadMs", c.geminiUploadMs);
  cJSON_AddNumberToObject(root, "transcribeMs", c.transcribeMs);
  cJSON_AddNumberToObject(root, "markdownMs", c.markdownMs);
  cJSON_AddNumberToObject(root, "driveUploadMs", c.driveUploadMs);
  cJSON_AddNumberToObject(root, "verifyingMs", c.verifyingMs);
  cJSON_AddNumberToObject(root, "quotaWaitMs", c.quotaWaitMs);
  cJSON_AddNumberToObject(root, "retryWaitMs", c.retryWaitMs);
  char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
  bool ok = json && NoteFiles::writeAtomic(sibling(entry.wavPath, ".perf"), json, strlen(json));
  free(json);
  if (!ok) { copy(work_.warning, "Falha ao salvar .perf; entrega preservada"); Serial.printf("[PerfWarning] %s: %s\n", entry.title, work_.warning); }
}

void SyncJob::taskEntry(void *arg) {
  static_cast<SyncJob *>(arg)->run(); vTaskDelete(nullptr);
}
void SyncJob::run() {
  Context *ctx = context_;
  SttClient::beginBatch();
  observerOwner_ = this;
  {
    History history; history.begin();
    char lastFailure[512] = "", lastWarning[96] = "";
    for (unsigned i = 0; i < ctx->count; ++i) {
      if (pauseRequested()) { work_.paused = true; break; }
      if (millis() - work_.startedAt >= kDeadlineMs) {
        copy(lastFailure, "Limite de 10 minutos atingido; notas restantes preservadas"); break;
      }
      Context::Entry &entry = ctx->entries[i];
      work_.currentNote = i + 1; copy(work_.noteLabel, entry.title);
      work_.error[0] = work_.warning[0] = work_.transcriptModel[0] = work_.markdownModel[0] = '\0';
      work_.delivered = false;
      bool hadTranscript = entry.hadTranscript, hadMarkdown = entry.hadMarkdown;
      uint32_t now = millis();
      SyncTelemetry::reset(observe);
      entry.sizes[1] = hadTranscript ? textSize(sibling(entry.wavPath, ".txt"), &entry.textChars) : 0;
      entry.sizes[2] = hadMarkdown ? textSize(sibling(entry.wavPath, ".md")) : 0;
      // Persisted offsets, sem rehash. O cliente valida identidade/geracao antes
      // de usa-los; geracao antiga nao recebe credito na barra.
      NoteSyncState state;
      if (ctx->hasDrive && state.load(entry.wavPath) && state.authGeneration == ctx->cfg.driveAuthGeneration) {
        if (state.wavSize == entry.wavBytes)
          entry.confirmed[0] = state.wavUploaded ? entry.wavBytes : state.wavBytesUploaded;
        if (hadTranscript && state.txtSize == entry.sizes[1])
          entry.confirmed[1] = state.txtUploaded ? state.txtSize : state.txtBytesUploaded;
        if (hadMarkdown && state.mdSize == entry.sizes[2])
          entry.confirmed[2] = state.mdUploaded ? state.mdSize : state.mdBytesUploaded;
      }
      uint64_t driveBytes = 0;
      for (unsigned asset = 0; asset < 3; ++asset)
        driveBytes += entry.sizes[asset] - std::min(entry.sizes[asset], entry.confirmed[asset]);
      uint32_t predictedChars = hadTranscript ? entry.textChars :
          bounded(static_cast<uint64_t>(entry.audioMs) * 14 / 1000, 0, SttClient::kMaxTextLen - 1);
      auto prediction = history.predict(ctx->cfg, entry.audioMs, entry.wavBytes, predictedChars, driveBytes);
      if (!ctx->hasStt) prediction.geminiUploadMs = prediction.transcribeMs = prediction.markdownMs = 0;
      if (!ctx->hasDrive) prediction.driveUploadMs = 0;
      work_.progress.begin(prediction, now, hadTranscript || !ctx->hasStt,
                           hadMarkdown || !ctx->hasStt, ctx->hasDrive);
      work_.progress.setDriveTotals(entry.sizes, entry.confirmed);
      SyncTelemetry::phase(SyncTelemetry::Phase::Preparing, entry.wavPath);
      bool success = processNote(i);
      if (success) ++work_.completedNotes;
      else if (!work_.paused) { ++work_.failed; copy(lastFailure, work_.error); }
      if (!work_.paused) ++work_.processedNotes;
      SyncTelemetry::phase(success ? SyncTelemetry::Phase::Done : SyncTelemetry::Phase::Error,
                           work_.paused ? "pausa apos etapa" : (success ? "nota confirmada" : work_.error));
      work_.counters = SyncTelemetry::counters();
      if (success) history.learn(work_.counters, entry.audioMs, entry.textChars, hadTranscript, hadMarkdown, work_);
      if (!history.open || history.openWarning) copy(work_.warning, "Historico NVS indisponivel; estimativa padrao");
      saveReport(i, success, now);
      if (work_.warning[0]) copy(lastWarning, work_.warning);
      publish();
      if (work_.paused) break;
      vTaskDelay(1);
    }
    if (lastFailure[0]) copy(work_.error, lastFailure);
    if (lastWarning[0]) copy(work_.warning, lastWarning);
    // Entrega pode estar em 100%; active permanece true ate fim da limpeza.
    SyncTelemetry::setObserver(nullptr); observerOwner_ = nullptr;
    copy(work_.detail, "limpeza final"); publish();
    if (WiFi.status() == WL_CONNECTED) SttClient::cleanupPending(ctx->cfg);
    drive_.endSession(); SttClient::endSession();
  } // Preferences fecha no mesmo worker antes da entrega final ao main
  context_ = nullptr; ctx->~Context(); free(ctx);
  work_.finished = true;
  if (work_.paused) copy(work_.detail, "pausa apos etapa");
  publish(); // ultima operacao compartilhada; taskEntry apenas se autodeleta
}
