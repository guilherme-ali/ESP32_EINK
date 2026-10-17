#include "assistant_job.h"
#include "live_protocol.h"
#include "transport.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

namespace {
constexpr size_t kCaptureBytes = 16000 * 2 * 30; // immutable prefix, no overwrite
constexpr size_t kOutputBytes = 2 * 1024 * 1024;
constexpr size_t kMaxAudioBytes = 192 * 1024;
constexpr size_t kWifiPrebufferBytes = 24000; // 500 ms de PCM mono, 24 kHz/16 bits.
constexpr uint32_t kTurnMs = 180000;
void copyText(char *to, size_t cap, const char *from) {
  if (!cap) return;
  size_t n = std::min(strlen(from), cap - 1);
  // Do not cut a multi-byte UTF-8 sequence at the UI boundary.
  while (n && (static_cast<unsigned char>(from[n]) & 0xc0) == 0x80) --n;
  memcpy(to, from, n); to[n] = 0;
}
bool bounded(const char *s, size_t capacity) { return memchr(s, 0, capacity) != nullptr; }
} // namespace

struct AssistantJob::Impl {
  AssistantJob *owner;
  AudioCodec *codec;
  ConnectFn connect;
  std::string key, model, gateway, token, zone;
  uint8_t *capture = nullptr, *incoming = nullptr, *output = nullptr;
  std::atomic<size_t> captured{0}, produced{0}, consumed{0};
  std::atomic<bool> released{false}, cancelled{false}, captureDone{false};
  std::atomic<bool> launched{false}, networkDone{false}, workerStop{false}, failed{false};
  std::atomic<bool> limited{false};
  std::atomic<bool> bufferedAudio{false};
  std::atomic<uint32_t> rebufferCount{0};
  TaskHandle_t audioHandle = nullptr, networkHandle = nullptr, gatewayHandle = nullptr;
  uint32_t started = 0;
  uint64_t nonce = 0;
  bool tools = false;
  float playbackVolume = 20;
  struct Call {
    std::string id, name, args, gatewayId, payload, result;
    bool cancelled = false, sent = false, submitted = false;
  };
  std::vector<Call> calls; // network task only; stable indices, max 16
  std::vector<std::string> cancelledIds;
  // SPSC handoff; release/acquire publishes strings before the worker reads.
  enum Work { Empty, Pending, Running, Complete };
  std::atomic<int> work{Empty};
  std::atomic<bool> workCancelled{false};
  std::string workPayload, workId, workResult;
  size_t workIndex = 0;
  explicit Impl(AssistantJob *o, AudioCodec *c, ConnectFn fn) : owner(o), codec(c), connect(fn) {}
  ~Impl() {
    if (!key.empty()) std::fill(key.begin(), key.end(), '\0');
    if (!token.empty()) std::fill(token.begin(), token.end(), '\0');
    if (!workPayload.empty()) std::fill(workPayload.begin(), workPayload.end(), '\0');
    for (auto &call : calls) std::fill(call.payload.begin(), call.payload.end(), '\0');
    if (capture) { memset(capture, 0, kCaptureBytes); free(capture); }
    free(incoming); free(output);
  }
  static bool stopped(void *p) {
    Impl *s = static_cast<Impl *>(p);
    return s->cancelled.load() || s->failed.load() || uint32_t(millis() - s->started) >= kTurnMs;
  }
  static bool toolStopped(void *p) {
    Impl *s = static_cast<Impl *>(p);
    return stopped(p) || s->workerStop.load() || s->workCancelled.load();
  }
  uint32_t remainingMs() const {
    const uint32_t elapsed = uint32_t(millis() - started);
    return elapsed < kTurnMs ? kTurnMs - elapsed : 0;
  }
  void fail(const char *message) {
    failed = true; released = true;
    owner->status(State::Failed, "Assistente interrompido.", message);
  }
  bool queueAudio(const char *data) {
    std::vector<uint8_t> pcm;
    if (!Assistant::unbase64(data, strlen(data), pcm, kMaxAudioBytes) || (pcm.size() & 1)) {
      fail("Áudio Gemini inválido ou acima do limite de mensagem."); return false;
    }
    size_t offset = 0; const uint32_t begin = millis();
    while (offset < pcm.size() && !stopped(this)) {
      size_t p = produced.load(), c = consumed.load();
      size_t room = kOutputBytes - (p - c);
      size_t take = std::min(pcm.size() - offset, room);
      take = std::min(take, kOutputBytes - p % kOutputBytes);
      if (take) {
        memcpy(output + p % kOutputBytes, pcm.data() + offset, take);
        produced.store(p + take, std::memory_order_release); offset += take;
      } else {
        if (!captureDone.load()) { fail("Resposta antecipada excedeu o buffer; solte o botão e tente novamente."); return false; }
        if (uint32_t(millis() - begin) >= 6000) { fail("Timeout no buffer de reprodução."); return false; }
        vTaskDelay(1);
      }
    }
    return offset == pcm.size();
  }
  bool message(const uint8_t *data, size_t length, bool &setupComplete, bool &turnComplete, bool &response) {
    using namespace Assistant;
    Json json = parseJson(reinterpret_cast<const char *>(data), length);
    if (!json) { fail("Mensagem Gemini inválida ou acima dos limites JSON."); return false; }
    const cJSON *root = json.get();
    if (field(root, "error")) { fail("Gemini retornou erro. Verifique modelo, chave, acesso gratuito e cota."); return false; }
    if (field(root, "setupComplete")) setupComplete = true;
    const cJSON *cancellation = field(field(root, "toolCallCancellation"), "ids");
    if (cJSON_IsArray(cancellation)) for (const cJSON *id = cancellation->child; id; id = id->next) {
      if (!cJSON_IsString(id)) continue;
      if (std::find(cancelledIds.begin(), cancelledIds.end(), id->valuestring) == cancelledIds.end()) {
        if (strlen(id->valuestring) > 256 || cancelledIds.size() >= 16) {
          fail("Limite de cancelamentos de ferramentas excedido."); return false;
        }
        cancelledIds.emplace_back(id->valuestring);
      }
      for (size_t i = 0; i < calls.size(); ++i) if (calls[i].id == id->valuestring) {
        calls[i].cancelled = true;
        if (calls[i].submitted && work.load() != Empty && workIndex == i) workCancelled = true;
      }
    }
    const cJSON *fc = field(field(root, "toolCall"), "functionCalls");
    if (cJSON_IsArray(fc)) for (const cJSON *call = fc->child; call; call = call->next) {
      std::string id = stringField(call, "id"), name = stringField(call, "name");
      const cJSON *args = field(call, "args");
      if (id.empty() || id.size() > 256 || name.empty() || name.size() > 64) {
        fail("Chamada de ferramenta Gemini inválida."); return false;
      }
      std::string arguments = cJSON_IsObject(args) ? printJson(args) : "{}";
      const bool oversizedArgs = arguments.empty() || arguments.size() > kGatewayLimit;
      if (oversizedArgs) arguments.clear(); // Do not retain 256-KiB rejected args per call.
      bool duplicate = false;
      for (auto &old : calls) if (old.id == id) {
        if (old.name != name || old.args != arguments) { fail("Gemini reutilizou id com argumentos diferentes."); return false; }
        old.sent = false; duplicate = true; break;
      }
      if (duplicate) continue;
      if (calls.size() == 16) { fail("Limite de 16 ferramentas por turno atingido."); return false; }
      Call entry; entry.id = id; entry.name = name; entry.args = arguments;
      entry.cancelled = std::find(cancelledIds.begin(), cancelledIds.end(), id) != cancelledIds.end();
      entry.gatewayId = Assistant::gatewayId(nonce, static_cast<unsigned>(calls.size()));
      if (oversizedArgs) entry.result = toolError("INVALID_ARGS", "Argumentos acima do limite de 32 KiB.");
      else if (!cJSON_IsObject(args)) entry.result = toolError("INVALID_ARGS", "Argumentos da ferramenta devem ser um objeto JSON.");
      else if (!knownTool(name)) entry.result = toolError(name == "web_search" ? "TOOL_DISABLED" : "UNKNOWN_TOOL",
        name == "web_search" ? "Consulta web não está habilitada." : "Ferramenta não disponível.");
      else if (!tools) entry.result = toolError("GATEWAY_CONFIG", "Gateway não configurado.");
      else {
        entry.payload = gatewayRequest(token, entry.gatewayId, name, arguments);
        if (entry.payload.empty()) entry.result = toolError("INVALID_ARGS", "Argumentos inválidos ou pedido acima de 32 KiB.");
      }
      calls.push_back(std::move(entry));
    }
    const cJSON *content = field(root, "serverContent");
    if (content) {
      if (trueField(content, "interrupted")) {
        // Single PTT turn: a server interruption cannot be mistaken for success.
        fail("Resposta interrompida pelo servidor."); return false;
      }
      const char *input = stringField(field(content, "inputTranscription"), "text");
      if (*input) owner->appendText(input);
      const char *transcript = stringField(field(content, "outputTranscription"), "text");
      if (*transcript) { owner->appendText(transcript); response = true; }
      const cJSON *parts = field(field(content, "modelTurn"), "parts");
      if (cJSON_IsArray(parts)) for (const cJSON *part = parts->child; part; part = part->next) {
        const char *text = stringField(part, "text");
        if (*text && !trueField(part, "thought") && !*transcript) { owner->appendText(text); response = true; }
        const cJSON *audio = field(part, "inlineData");
        if (audio) {
          const char *mime = stringField(audio, "mimeType"), *encoded = stringField(audio, "data");
          if (strcmp(mime, "audio/pcm;rate=24000") && strcmp(mime, "audio/pcm")) {
            fail("Formato de áudio Gemini não suportado (esperado PCM 24 kHz)."); return false;
          }
          if (!queueAudio(encoded)) return false;
          if (*encoded) response = true;
        }
      }
      if (trueField(content, "turnComplete")) turnComplete = true;
    }
    return true;
  }
};

void AssistantJob::status(State state, const char *detail, const char *error) {
  portENTER_CRITICAL(&mux_);
  if (view_.state != State::Failed || state == State::Failed) {
    view_.state = state; copyText(view_.detail, sizeof(view_.detail), detail);
    if (error) copyText(view_.error, sizeof(view_.error), error);
  }
  portEXIT_CRITICAL(&mux_);
}
void AssistantJob::appendText(const char *text) {
  std::string safe = Assistant::redact(text, impl_->key, impl_->token);
  portENTER_CRITICAL(&mux_);
  size_t n = strlen(view_.text), room = sizeof(view_.text) - n;
  if (room > 1) copyText(view_.text + n, room, safe.c_str());
  portEXIT_CRITICAL(&mux_);
}
bool AssistantJob::start(const Settings &s, AudioCodec &codec, ConnectFn connect) {
  reap();
  if (impl_) return false;
  portENTER_CRITICAL(&mux_); view_ = Snapshot{}; portEXIT_CRITICAL(&mux_);
  if (!s.assistantEnabled || !bounded(s.assistantModel, sizeof(s.assistantModel)) ||
      !bounded(s.sttApiKey, sizeof(s.sttApiKey)) || !*s.sttApiKey ||
      !bounded(s.assistantGatewayUrl, sizeof(s.assistantGatewayUrl)) ||
      !bounded(s.assistantGatewayToken, sizeof(s.assistantGatewayToken)) ||
      !bounded(s.assistantTimeZone, sizeof(s.assistantTimeZone)) ||
      !Assistant::allowedModel(s.assistantModel, s.geminiFreeConfirmed) || !connect) {
    status(State::Failed, "Configuração do assistente inválida.",
      "Ative o assistente, configure um modelo Live permitido, chave e confirme acesso gratuito.");
    portENTER_CRITICAL(&mux_); view_.finished = true; portEXIT_CRITICAL(&mux_); return false;
  }
  Impl *p = new (std::nothrow) Impl(this, &codec, connect);
  if (!p) { status(State::Failed, "Sem memória.", "Falha ao alocar sessão do assistente.");
    portENTER_CRITICAL(&mux_); view_.finished = true; portEXIT_CRITICAL(&mux_); return false; }
  impl_ = p;
  p->key = s.sttApiKey; p->model = s.assistantModel; p->gateway = s.assistantGatewayUrl;
  p->token = s.assistantGatewayToken; p->zone = s.assistantTimeZone;
  p->playbackVolume = s.speakerVolumeDb;
  std::string path;
  p->tools = Assistant::gatewayOrigin(p->gateway, path) && p->token.size() >= 32 && p->token.size() <= 128;
  p->capture = static_cast<uint8_t *>(heap_caps_malloc(kCaptureBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  p->incoming = static_cast<uint8_t *>(heap_caps_malloc(Assistant::kJsonLimit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  p->output = static_cast<uint8_t *>(heap_caps_malloc(kOutputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  p->started = millis(); esp_fill_random(&p->nonce, sizeof(p->nonce));
  if (!p->capture || !p->incoming || !p->output || !codec.setSampleRate(16000) ||
      !codec.enable(true) || !codec.setMicGain(s.micGainDb) || !codec.setVolume(0)) {
    p->fail("Sem PSRAM ou falha ao configurar codec em 16 kHz."); codec.enable(false);
    delete p; impl_ = nullptr;
    portENTER_CRITICAL(&mux_); view_.finished = true; portEXIT_CRITICAL(&mux_); return false;
  }
  status(State::Connecting, "Ouvindo enquanto conecta...");
  // All tasks start behind a gate, so handles exist before any task can retire.
  bool ok = xTaskCreatePinnedToCore(audioTask, "as_audio", 6144, p, 5, &p->audioHandle, 0) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(gatewayTask, "as_tools", 12288, p, 2, &p->gatewayHandle, 1) == pdPASS;
  if (ok) ok = xTaskCreatePinnedToCore(networkTask, "as_live", 16384, p, 3, &p->networkHandle, 1) == pdPASS;
  if (!ok) {
    p->fail("Falha ao criar tasks do assistente."); p->cancelled = true; p->workerStop = true; p->networkDone = true;
    if (!p->audioHandle) { p->captureDone = true; codec.enable(false); }
  }
  p->launched = true;
  return ok;
}
void AssistantJob::release() {
  portENTER_CRITICAL(&mux_);
  if (impl_) impl_->released = true;
  view_.released = true;
  portEXIT_CRITICAL(&mux_);
}
void AssistantJob::cancel() {
  portENTER_CRITICAL(&mux_);
  if (impl_) { impl_->cancelled = true; impl_->released = true; }
  view_.released = true;
  portEXIT_CRITICAL(&mux_);
}
void AssistantJob::reap() {
  Impl *p = impl_;
  if (!p) return;
  for (TaskHandle_t h : {p->audioHandle, p->networkHandle, p->gatewayHandle})
    if (h && eTaskGetState(h) != eSuspended) return;
  // A suspended task has left its implementation/stack scope and will never
  // touch Impl again. Delete it here before publishing finished or freeing RAM.
  for (TaskHandle_t *h : {&p->audioHandle, &p->networkHandle, &p->gatewayHandle})
    if (*h) { vTaskDelete(*h); *h = nullptr; }
  portENTER_CRITICAL(&mux_);
  view_.capturedMs = static_cast<uint32_t>(p->captured.load() / 32);
  view_.receivedAudioMs = static_cast<uint32_t>(p->produced.load() / 48);
  view_.playedAudioMs = static_cast<uint32_t>(p->consumed.load() / 48);
  view_.rebufferCount = p->rebufferCount.load(); view_.bufferedAudio = p->bufferedAudio.load();
  view_.released = p->released.load(); view_.finished = true;
  if (view_.state != State::Failed) {
    view_.state = p->cancelled ? State::Failed : State::Done;
    copyText(view_.detail, sizeof(view_.detail), p->cancelled ? "Cancelado." : "Concluído.");
    if (p->limited && !p->cancelled) copyText(view_.detail, sizeof(view_.detail), "Concluído; captura interrompida no limite de 30 s.");
    if (p->cancelled) copyText(view_.error, sizeof(view_.error), "Conversa cancelada.");
  }
  portEXIT_CRITICAL(&mux_);
}
bool AssistantJob::active() const {
  const_cast<AssistantJob *>(this)->reap();
  portENTER_CRITICAL(&mux_); bool result = impl_ && !view_.finished; portEXIT_CRITICAL(&mux_); return result;
}
bool AssistantJob::snapshot(Snapshot &out) const {
  const_cast<AssistantJob *>(this)->reap();
  portENTER_CRITICAL(&mux_);
  out = view_;
  if (impl_ && !out.finished) {
    out.capturedMs = static_cast<uint32_t>(impl_->captured.load() / 32); out.released = impl_->released.load();
    out.receivedAudioMs = static_cast<uint32_t>(impl_->produced.load() / 48);
    out.playedAudioMs = static_cast<uint32_t>(impl_->consumed.load() / 48);
    out.rebufferCount = impl_->rebufferCount.load(); out.bufferedAudio = impl_->bufferedAudio.load();
  }
  portEXIT_CRITICAL(&mux_); return true;
}
bool AssistantJob::acknowledgeFinished() {
  reap();
  portENTER_CRITICAL(&mux_);
  if (!view_.finished) { portEXIT_CRITICAL(&mux_); return false; }
  Impl *p = impl_; impl_ = nullptr; view_ = Snapshot{};
  portEXIT_CRITICAL(&mux_);
  delete p; return true;
}

void AssistantJob::audioTask(void *ptr) {
  {
    Impl *p = static_cast<Impl *>(ptr);
    while (!p->launched.load()) vTaskDelay(1);
    uint8_t stereo[2048]; size_t carry = 0;
    uint32_t lastData = millis(), captureStart = millis();
    while (!p->released.load() && !Impl::stopped(p)) {
      if (uint32_t(millis() - captureStart) >= 30000) {
        p->limited = true; p->released = true;
        p->owner->status(State::Thinking, "Captura interrompida no limite de 30 s."); break;
      }
      size_t n = 0;
      bool read = p->codec->read(stereo + carry, sizeof(stereo) - carry, &n, 40);
      if (!read || n > sizeof(stereo) - carry) { p->fail("Falha de leitura do microfone."); break; }
      if (!n) {
        if (uint32_t(millis() - lastData) > 2000) { p->fail("Microfone não forneceu áudio."); break; }
        vTaskDelay(1); continue;
      }
      lastData = millis(); n += carry;
      size_t frames = n / 4, accepted = p->captured.load();
      size_t take = std::min(frames, (kCaptureBytes - accepted) / 2);
      for (size_t i = 0; i < take; ++i) {
        p->capture[accepted + i * 2] = stereo[i * 4];
        p->capture[accepted + i * 2 + 1] = stereo[i * 4 + 1];
      }
      p->captured.store(accepted + take * 2, std::memory_order_release);
      carry = n % 4; if (carry) memmove(stereo, stereo + n - carry, carry);
      if (accepted + take * 2 == kCaptureBytes) {
        p->limited = true; p->released = true;
        p->owner->status(State::Thinking, "Captura interrompida no limite de 30 s."); break;
      }
      vTaskDelay(1);
    }
    if (carry) p->fail("Microfone encerrou com frame estéreo incompleto.");
    p->captureDone.store(true, std::memory_order_release);
    bool playback = false, feeding = false, buffering = false; uint32_t lastWrite = 0;
    while (!Impl::stopped(p)) {
      const bool done = p->networkDone.load(std::memory_order_acquire);
      size_t c = p->consumed.load(), available = p->produced.load(std::memory_order_acquire) - c;
      if (!available) {
        if (done) break;
        if (feeding) { feeding = false; ++p->rebufferCount; }
      }
      // BLE nao garante os 48 kB/s do PCM. Prefere fala continua a tocar cada
      // fragmento assim que chega. Respostas > buffer ainda progridem sem deadlock.
      const size_t prebuffer = p->bufferedAudio.load() ? kOutputBytes - kMaxAudioBytes : kWifiPrebufferBytes;
      if (!feeding && !done && available < prebuffer) {
        if (!buffering && p->bufferedAudio.load()) p->owner->status(State::Thinking, "Recebendo resposta pelo celular...");
        buffering = true;
        vTaskDelay(1); continue;
      }
      if (!available) continue;
      if (!feeding && playback) p->owner->status(State::Speaking, "Respondendo...");
      feeding = true; buffering = false;
      if (!playback) {
        if (!p->codec->setSampleRate(24000) || !p->codec->setVolume(p->playbackVolume)) {
          p->fail("Codec recusou reprodução em 24 kHz ou volume."); break;
        }
        playback = true; p->owner->status(State::Speaking, "Respondendo...");
      }
      size_t samples = std::min(available / 2, size_t(256));
      for (size_t i = 0; i < samples; ++i) {
        uint8_t lo = p->output[(c + i * 2) % kOutputBytes], hi = p->output[(c + i * 2 + 1) % kOutputBytes];
        stereo[i * 4] = stereo[i * 4 + 2] = lo; stereo[i * 4 + 1] = stereo[i * 4 + 3] = hi;
      }
      size_t offset = 0, wanted = samples * 4; const uint32_t begin = millis();
      while (offset < wanted && !Impl::stopped(p)) {
        size_t written = 0;
        bool ok = p->codec->write(stereo + offset, wanted - offset, &written, 40);
        if (!ok || written > wanted - offset || uint32_t(millis() - begin) >= 2000) {
          p->fail("Falha ou timeout na reprodução de áudio."); break;
        }
        offset += written; if (!written) vTaskDelay(1);
        if (written) lastWrite = millis();
      }
      if (offset != wanted) break;
      p->consumed.store(c + samples * 2, std::memory_order_release); vTaskDelay(1);
    }
    // codec.write queues into 8 x 512-frame DMA buffers. Give the last accepted
    // samples 180 ms to leave DMA at 24 kHz before disabling the amplifier.
    while (playback && !Impl::stopped(p) && uint32_t(millis() - lastWrite) < 180) vTaskDelay(1);
    p->released = true;
    if (uint32_t(millis() - p->started) >= kTurnMs && !p->cancelled && !p->failed)
      p->fail("Tempo máximo de conversa (180 s) excedido durante reprodução.");
    if (!p->codec->enable(false) && !p->failed.load()) p->fail("Falha ao desligar codec.");
  }
  vTaskSuspend(nullptr); // Reaped/deleted by the UI owner; no self-deletion race.
}

void AssistantJob::gatewayTask(void *ptr) {
  {
    Impl *p = static_cast<Impl *>(ptr);
    while (!p->launched.load()) vTaskDelay(1);
    while (!p->workerStop.load() && !Impl::stopped(p)) {
      if (p->work.load(std::memory_order_acquire) != Impl::Pending) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
      p->work = Impl::Running;
      p->workResult = Assistant::ToolGateway::execute(p->gateway, p->workPayload, p->workId, Impl::toolStopped, p, p->remainingMs());
      {
        Assistant::Json result = Assistant::parseJson(p->workResult.data(), p->workResult.size(), Assistant::kGatewayLimit);
        const cJSON *error = Assistant::field(result.get(), "error");
        const char *code = Assistant::stringField(error, "code");
        // Exactly one reconciliation request, with the SAME serialized body/id.
        // Apps Script's durable ledger handles a lost reply after a mutation.
        // Redirect/auth/JSON errors are not retried. A cancellation forbids a
        // fresh POST, even if the earlier remote outcome remains uncertain.
        if (!Impl::toolStopped(p) && (!strcmp(code, "OUTCOME_UNKNOWN") || !strcmp(code, "REMOTE_UNCERTAIN")))
          p->workResult = Assistant::ToolGateway::execute(p->gateway, p->workPayload, p->workId, Impl::toolStopped, p, p->remainingMs());
      }
      {
        Assistant::Json result = Assistant::parseJson(p->workResult.data(), p->workResult.size(), Assistant::kGatewayLimit);
        if (result && !Assistant::trueField(result.get(), "ok") &&
            cJSON_AddStringToObject(result.get(), "requestId", p->workId.c_str()))
          p->workResult = Assistant::printJson(result.get());
      }
      p->work.store(Impl::Complete, std::memory_order_release);
    }
  }
  vTaskSuspend(nullptr);
}

void AssistantJob::networkTask(void *ptr) {
  {
    using namespace Assistant;
    Impl *p = static_cast<Impl *>(ptr);
    while (!p->launched.load()) vTaskDelay(1);
    LiveSocket socket(p->incoming, kJsonLimit + 1, Impl::stopped, p);
    bool setup = false, complete = false, response = false, ended = false, started = false;
    size_t sent = 0; uint32_t setupBegin = millis(), endAt = 0;
    bool ready = !Impl::stopped(p) && p->connect();
    if (!ready && !Impl::stopped(p)) p->fail("Sem conexão Wi-Fi ou ponte TLS pelo celular.");
    if (ready && !Impl::stopped(p)) ready = socket.connect(p->key.c_str());
    if (!ready && !Impl::stopped(p)) p->fail(socket.error());
    if (ready && !Impl::stopped(p)) {
      p->bufferedAudio = NetworkRoute::phoneSelected();
      std::string json = setupJson(p->model.c_str(), true, p->zone.c_str(), static_cast<int64_t>(time(nullptr)), p->tools);
      ready = socket.send(json); setupBegin = millis();
      if (!ready) p->fail(socket.error());
    }
    while (ready && !Impl::stopped(p)) {
      // Keep receiving during recording and gateway I/O. At most 16 messages per
      // scheduling slice; bounded framing work prevents starving audio/TX.
      for (unsigned i = 0; i < 16 && !Impl::stopped(p); ++i) {
        auto event = socket.poll();
        if (event == WsParser::Event::Error || event == WsParser::Event::Close) { p->fail(socket.error()); break; }
        if (event != WsParser::Event::Message) break;
        if (!p->message(socket.data(), socket.size(), setup, complete, response)) break;
      }
      if (Impl::stopped(p)) break;
      if (!setup) {
        if (uint32_t(millis() - setupBegin) >= 15000) { p->fail("Timeout aguardando setupComplete do Gemini."); break; }
        vTaskDelay(1); continue;
      }
      if (!started) {
        if (!socket.send(activityJson(true))) { p->fail(socket.error()); break; }
        started = true;
        p->owner->status(p->released ? State::Thinking : State::Listening,
          p->released ? "Enviando fala preservada..." : "Ouvindo; solte para responder.");
      }
      size_t available = p->captured.load(std::memory_order_acquire) - sent;
      if (available >= 2048 || (p->captureDone.load(std::memory_order_acquire) && available)) {
        size_t n = std::min(available, size_t(2048)); // 64 ms; final tail may be shorter.
        if (!socket.send(audioJson(p->capture + sent, n))) { p->fail(socket.error()); break; }
        sent += n;
      }
      if (!ended && p->captureDone.load(std::memory_order_acquire) && sent == p->captured.load()) {
        if (!sent) { p->fail("Nenhum áudio foi capturado."); break; }
        if (!socket.send(activityJson(false))) { p->fail(socket.error()); break; }
        ended = true; endAt = millis(); p->owner->status(State::Thinking, "Aguardando resposta...");
      }
      if (p->work.load(std::memory_order_acquire) == Impl::Complete) {
        auto &call = p->calls[p->workIndex];
        call.result = p->workResult;
        p->workResult.clear(); p->workPayload.assign(p->workPayload.size(), '\0'); p->workPayload.clear();
        p->work.store(Impl::Empty, std::memory_order_release);
      }
      bool toolsPending = false;
      for (size_t i = 0; i < p->calls.size(); ++i) {
        auto &call = p->calls[i];
        if (call.cancelled && !call.submitted && call.result.empty())
          call.result = toolError("CANCELLED", "Ferramenta cancelada antes de iniciar efeitos.");
        if (!call.result.empty() && !call.sent) {
          std::string json = toolResponseJson(call.id, call.name, call.result);
          if (!socket.send(json)) { p->fail(socket.error()); break; }
          call.sent = true; complete = false;
        }
        if (!call.sent) toolsPending = true;
        if (!call.submitted && call.result.empty() && p->work.load() == Impl::Empty && !socket.inputPending()) {
          // Give cancellation frames a full receive slice before starting effects.
          p->workIndex = i; p->workPayload = call.payload; p->workId = call.gatewayId;
          p->workCancelled = call.cancelled; call.submitted = true;
          p->work.store(Impl::Pending, std::memory_order_release);
        }
      }
      if (ended && complete && response && !toolsPending && p->work.load() == Impl::Empty) break;
      // Tool work can precede the first audio; all are still bounded by 180 s.
      if (ended && !response && !toolsPending && uint32_t(millis() - endAt) >= 45000) {
        p->fail("Gemini não enviou áudio nem texto em 45 s após a fala."); break;
      }
      if (ended && complete && !response && !toolsPending) { p->fail("Gemini encerrou o turno sem áudio ou texto."); break; }
      vTaskDelay(1);
    }
    if (uint32_t(millis() - p->started) >= kTurnMs && !p->failed.load() && !p->cancelled.load())
      p->fail("Tempo máximo de conversa (180 s) excedido.");
    p->released = true; p->workerStop = true;
    socket.close();
    p->networkDone.store(true, std::memory_order_release);
  }
  vTaskSuspend(nullptr);
}
