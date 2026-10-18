#include "app.h"
#include "../ui/screens.h"
#include "../ui/wallpapers.h"
#include "../board/battery.h"
#include <LittleFS.h>
#include "../storage/note_files.h"
#include <esp_heap_caps.h>
#include "../connectivity/network_route.h"
#include "../connectivity/ble_relay.h"
#include "../net/gemini_client.h"
#include "../assistant/live_protocol.h"

// Contrato do widget fornecido pela integracao principal em ui/screens.*.
namespace Screens {
void drawSyncProgress(Canvas &, EPaperDisplay &, const char *noteLabel,
                      const char *phaseLabel, unsigned percent, bool estimated,
                      int currentNote, int completedNotes, int totalNotes,
                      uint32_t elapsedMs, uint32_t remainingMs, bool overdue);
}

namespace {
struct DiagnosticDone {
  const String &command;
  bool enabled = true;
  explicit DiagnosticDone(const String &value) : command(value) {}
  ~DiagnosticDone() { if (enabled) Serial.printf("[DiagDone] %s\n", command.c_str()); }
};

void setVal(char *dst, size_t n, const char *src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

void setItem(MenuItem &item, const char *label) {
  item.label = label;
  item.value[0] = '\0';
}

template <typename T>
int cycleIndex(const T *arr, int n, T current) {
  int idx = 0;
  for (int i = 0; i < n; i++) {
    if (arr[i] == current) { idx = i; break; }
  }
  return (idx + 1) % n;
}

const uint32_t kSampleRates[] = {8000, 16000, 32000, 44100, 48000};
constexpr int kSampleRateCount = sizeof(kSampleRates) / sizeof(kSampleRates[0]);

const float kMicGains[] = {0, 6, 12, 18, 24, 30, 36, 42};
constexpr int kMicGainCount = sizeof(kMicGains) / sizeof(kMicGains[0]);

const float kVolumes[] = {0, 20, 40, 60, 80, 100};
constexpr int kVolumeCount = sizeof(kVolumes) / sizeof(kVolumes[0]);

const uint16_t kScreensaverTimeouts[] = {30, 60, 120, 300, 600};
constexpr int kScreensaverTimeoutCount = sizeof(kScreensaverTimeouts) / sizeof(kScreensaverTimeouts[0]);

const char *const kWallpaperNames[] = {"Montanhas", "Topo", "Estrelas", "Ondas", "Pontos"};

enum RootIdx { kRootRecord = 0, kRootNotes, kRootSync, kRootPhone, kRootWifi, kRootSettings, kRootAbout, kRootCount };

enum NoteDetailIdx {
  kNdSummary = 0,
  kNdTranscript,
  kNdPlay,
  kNdSync,
  kNdObsidian,
  kNdDelete,
  kNdBack,
  kNoteDetailCount
};

enum SettingsIdx {
  kSetAudio = 0,
  kSetMicGain,
  kSetVolume,
  kSetScreensaver,
  kSetWallpaper,
  kSetShowTemp,
  kSetAutoSync,
  kSetDeleteAll,
  kSetBack,
  kSettingsItemCount
};

} // namespace

App::App(EPaperDisplay &epd, Canvas &canvas, NotesStore &notes, SettingsStore &settingsStore,
         AudioCodec &codec, Recorder &recorder, Player &player, WifiManager &wifiMgr,
         SttClient &sttClient, GDriveClient &gdrive, Rtc &rtc, Shtc3 &shtc3)
    : epd_(epd), canvas_(canvas), notes_(notes), settingsStore_(settingsStore), codec_(codec),
      recorder_(recorder), player_(player), wifiMgr_(wifiMgr), sttClient_(sttClient),
      gdrive_(gdrive), rtc_(rtc), shtc3_(shtc3), syncJob_(settingsStore, sttClient, gdrive),
      menu_(canvas, epd), keyboard_(canvas, epd) {}

void App::begin(SleepRequestFn onSleepRequested, ConnectRequestFn onConnectRequested,
                 PortalRequestFn onPortalRequested, ConnectRequestFn onAssistantConnectRequested) {
  onSleepRequested_ = onSleepRequested;
  onConnectRequested_ = onConnectRequested;
  onPortalRequested_ = onPortalRequested;
  onAssistantConnectRequested_ = onAssistantConnectRequested;
  refreshNotes();
  screen_ = Screen::Home;
  drawHome();
  markActivity();
}

void App::markActivity() { lastActivityMs_ = millis(); }

uint32_t App::bytesPerSec() const { return settingsStore_.get().audioSampleRateHz * 2; }

void App::formatDuration(uint32_t bytes, uint32_t sampleRateHz, char *out, size_t outLen) const {
  if (!sampleRateHz || bytes <= sizeof(WavHeader)) { snprintf(out, outLen, "sem audio"); return; }
  uint32_t bps = sampleRateHz * 2;
  float secs = (bytes - sizeof(WavHeader)) / (float)bps;
  snprintf(out, outLen, "%.0fs", secs);
}

void App::refreshNotes() {
  // Tambem cobre pareamento iniciado pelo menu Wi-Fi, fora de startSync().
  uint32_t generation = settingsStore_.get().driveAuthGeneration;
  if (generation != notesDriveGeneration_) {
    notes_.markDirty(); notesDriveGeneration_ = generation;
  }
  noteCount_ = notes_.count();
  if (notesSel_ >= noteCount_) notesSel_ = max(0, noteCount_ - 1);
}

// ---------------------------------------------------------------------
// Navegacao
// ---------------------------------------------------------------------

void App::goHome() {
  screen_ = Screen::Home;
  refreshNotes();
  drawHome();
}

void App::goRootMenu() {
  refreshNotes();
  screen_ = Screen::RootMenu;
  drawRootMenu();
}

void App::goNotesList() {
  refreshNotes();
  screen_ = Screen::NotesList;
  drawNotesList();
}

void App::goNoteDetail(int index) {
  notesSel_ = index;
  noteDetailSel_ = 0;
  screen_ = Screen::NoteDetail;
  drawNoteDetail();
}

void App::goSettings() {
  screen_ = Screen::Settings;
  drawSettings();
}

void App::goAbout() {
  screen_ = Screen::About;
  Screens::drawAbout(canvas_, epd_, noteCount_, notes_.usedBytes(), notes_.totalBytes());
}

void App::goWifiMenu() {
  screen_ = Screen::WifiMenu;
  drawWifiMenu();
}

// Liga o radio e conecta sob demanda, se ainda nao estiver conectado.
// Bloqueante - desenha sua propria tela de espera.
bool App::ensureOnline() {
  if (NetworkRoute::online() && (!settingsStore_.hasDriveApp() || settingsStore_.hasDriveAuth())) return true;
  Screens::drawState(canvas_, epd_, Screens::StateIcon::Wifi, "conectando",
                      "Wi-Fi primeiro; celular BLE como alternativa");
  return onConnectRequested_ ? onConnectRequested_() : false;
}

// Nenhuma rede salva estava por perto - avisa e volta pra tela inicial
// sozinho (em vez de deixar o usuario preso numa tela de sincronizar
// que nao vai a lugar nenhum sem Wi-Fi).
void App::reportNoNetworkAndGoHome() {
  Screens::drawText(canvas_, epd_, "Sincronizar",
                     "Sem acesso a internet. Confira o Wi-Fi ou o celular pareado com Bluetooth e dados ativos. "
                     "A nota continua salva.");
  delay(2500);
  goHome();
}

// ---------------------------------------------------------------------
// Desenho de cada tela
// ---------------------------------------------------------------------

void App::drawHome() {
  RtcDateTime now;
  bool timeValid = rtc_.getDateTime(now) && now.year >= 2024;
  bool hasDrive = settingsStore_.hasDriveAuth();
  bool hasStt = (settingsStore_.get().sttEndpoint[0] != '\0');
  int pending = notes_.countPendingSync(hasStt, hasDrive);
  int batteryPercent = Battery::readPercent();

  float tempC = 0, humidity = 0;
  bool hasTempHumidity = shtc3_.read(tempC, humidity);

  Screens::drawHome(canvas_, epd_, now, timeValid, batteryPercent,
                     settingsStore_.get().showTempHumidity, hasTempHumidity, tempC, humidity,
                     noteCount_, pending);
}

void App::drawRootMenu() {
  bool hasDrive = settingsStore_.hasDriveAuth();
  bool hasStt = (settingsStore_.get().sttEndpoint[0] != '\0');
  int pending = notes_.countPendingSync(hasStt, hasDrive);
  MenuItem items[kRootCount];
  setItem(items[kRootRecord], "Gravar");
  setItem(items[kRootNotes], "Notas");
  snprintf(items[kRootNotes].value, sizeof(items[kRootNotes].value), "%d", noteCount_);
  setItem(items[kRootSync], "Sincronizar");
  if (pending > 0) {
    snprintf(items[kRootSync].value, sizeof(items[kRootSync].value), "%d", pending);
  } else if (!hasDrive && !hasStt) {
    strncpy(items[kRootSync].value, "bloq", sizeof(items[kRootSync].value) - 1);
  }
  setItem(items[kRootWifi], "Wi-Fi");
  setItem(items[kRootPhone], "Parear celular (BLE)");
  setItem(items[kRootSettings], "Configuracoes");
  setItem(items[kRootAbout], "Sobre");

  menu_.draw("Menu", items, kRootCount, rootSel_, "BOOT sel | PWR nav/volta");
}

void App::drawNotesList() {
  if (noteCount_ == 0) {
    Screens::drawText(canvas_, epd_, "Notas",
                       "Nenhuma nota ainda. Grave uma pelo menu ou pelo BOOT na tela inicial.",
                       "PWR volta");
    return;
  }

  constexpr int kMaxUi = 32;
  int shown = min(noteCount_, kMaxUi);
  NoteEntry entries[kMaxUi];
  for (int i = 0; i < shown; i++) notes_.getAt(i, entries[i]);

  MenuCard cards[kMaxUi];
  for (int i = 0; i < shown; i++) {
    char dur[8];
    formatDuration(entries[i].sizeBytes, entries[i].sampleRateHz, dur, sizeof(dur));
    snprintf(cards[i].line1, sizeof(cards[i].line1), "#%03d  %s", noteCount_ - i, dur);

    // label: "YYYYMMDD-HHMMSS" -> "YYYY-MM-DD HH:MM"
    const char *l = entries[i].label;
    if (strlen(l) >= 15) {
      snprintf(cards[i].line2, sizeof(cards[i].line2), "%.4s-%.2s-%.2s %.2s:%.2s", l, l + 4,
               l + 6, l + 9, l + 11);
    } else {
      setVal(cards[i].line2, sizeof(cards[i].line2), l);
    }
  }

  menu_.drawCards("notes", cards, shown, min(notesSel_, shown - 1), "BOOT abre | PWR nav/volta");
}

void App::drawNoteDetail() {
  NoteEntry e;
  if (!notes_.getAt(notesSel_, e)) {
    goNotesList();
    return;
  }

  MenuItem items[kNoteDetailCount];
  setItem(items[kNdSummary], "Ver resumo (.md)");
  setVal(items[kNdSummary].value, sizeof(items[kNdSummary].value), e.hasMd ? "ok" : "sem");
  setItem(items[kNdTranscript], "Ver transcricao");
  setVal(items[kNdTranscript].value, sizeof(items[kNdTranscript].value), e.hasTxt ? "ok" : "sem");
  setItem(items[kNdPlay], "Reproduzir");
  setItem(items[kNdSync], "Sincronizar esta");
  setItem(items[kNdObsidian], "Enviar ao Obsidian");
  setVal(items[kNdSync].value, sizeof(items[kNdSync].value), e.hasSnc ? "enviada" : "pendente");
  setItem(items[kNdDelete], "Apagar");
  setItem(items[kNdBack], "Voltar");

  menu_.draw(e.label, items, kNoteDetailCount, noteDetailSel_, "BOOT sel | PWR nav/volta");
}

void App::drawTextViewer() {
  Screens::drawPagedText(canvas_, epd_, textViewerTitle_, textViewerBuf_ ? textViewerBuf_ : "",
                         textViewerPage_, textViewerTotalPages_);
}

bool App::ensureTextViewerBuffer() {
  if (!textViewerBuf_) textViewerBuf_ = static_cast<char *>(heap_caps_malloc(
      kTextViewerMaxLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (textViewerBuf_) { textViewerBuf_[0] = '\0'; return true; }
  showSyncResult("PSRAM insuficiente para abrir o texto.", true);
  return false;
}

void App::drawSettings() {
  const Settings &cfg = settingsStore_.get();
  MenuItem items[kSettingsItemCount];

  setItem(items[kSetAudio], "Qualidade audio");
  snprintf(items[kSetAudio].value, sizeof(items[kSetAudio].value), "%lu Hz",
           (unsigned long)cfg.audioSampleRateHz);

  setItem(items[kSetMicGain], "Ganho microfone");
  snprintf(items[kSetMicGain].value, sizeof(items[kSetMicGain].value), "%ddB", (int)cfg.micGainDb);

  setItem(items[kSetVolume], "Volume");
  snprintf(items[kSetVolume].value, sizeof(items[kSetVolume].value), "%d%%", (int)cfg.speakerVolumePercent);

  setItem(items[kSetScreensaver], "Tempo p/ dormir");
  snprintf(items[kSetScreensaver].value, sizeof(items[kSetScreensaver].value), "%us",
           cfg.screensaverTimeoutSec);

  setItem(items[kSetWallpaper], "Plano de fundo");
  setVal(items[kSetWallpaper].value, sizeof(items[kSetWallpaper].value),
         cfg.wallpaperChoice < 0 ? "aleatorio" : kWallpaperNames[cfg.wallpaperChoice % WALLPAPER_COUNT]);

  setItem(items[kSetShowTemp], "Mostrar temp/umid");
  setVal(items[kSetShowTemp].value, sizeof(items[kSetShowTemp].value),
         cfg.showTempHumidity ? "sim" : "nao");

  setItem(items[kSetAutoSync], "Sincr. ao gravar");
  setVal(items[kSetAutoSync].value, sizeof(items[kSetAutoSync].value),
         cfg.autoSyncEnabled ? "sim" : "nao");

  setItem(items[kSetDeleteAll], "Apagar todas notas");
  setItem(items[kSetBack], "Voltar");

  menu_.draw("Configuracoes", items, kSettingsItemCount, settingsSel_, "BOOT altera | PWR nav/volta");
}

void App::drawWifiMenu() {
  const Settings &cfg = settingsStore_.get();
  constexpr int kMaxItems = Settings::kMaxWifiNetworks + 4;
  MenuItem items[kMaxItems];
  int n = 0;
  for (int i = 0; i < cfg.wifiNetworkCount; i++) {
    setItem(items[n], cfg.wifiSsid[i]);
    if (strcmp(cfg.wifiSsid[i], cfg.favoriteWifiSsid) == 0) {
      setVal(items[n].value, sizeof(items[n].value), "favorita");
    }
    n++;
  }
  setItem(items[n++], "+ Escanear redes");
  setItem(items[n++], "+ Adicionar manual");
  setItem(items[n++], "Portal (STT/Drive)");
  setItem(items[n++], "Voltar");

  menu_.draw("Wi-Fi", items, n, wifiMenuSel_, "BOOT sel | PWR nav/volta");
}

void App::drawWifiNetworkDetail() {
  const Settings &cfg = settingsStore_.get();
  bool isFavorite = strcmp(cfg.wifiSsid[wifiDetailIndex_], cfg.favoriteWifiSsid) == 0;

  MenuItem items[4];
  setItem(items[0], "Conectar agora");
  setItem(items[1], isFavorite ? "Favorita (tirar)" : "Marcar favorita");
  setItem(items[2], "Remover");
  setItem(items[3], "Voltar");
  menu_.draw(cfg.wifiSsid[wifiDetailIndex_], items, 4, wifiDetailSel_, "BOOT sel | PWR nav/volta");
}

void App::drawWifiScanList() {
  if (scanCount_ == 0) {
    Screens::drawText(canvas_, epd_, "Wi-Fi",
                       "Nenhuma rede nova encontrada por perto.", "PWR volta");
    return;
  }
  MenuItem items[kMaxScanResults + 1];
  for (int i = 0; i < scanCount_; i++) {
    setItem(items[i], scanResults_[i].ssid);
    snprintf(items[i].value, sizeof(items[i].value), "%ddBm", scanResults_[i].rssi);
  }
  setItem(items[scanCount_], "Voltar");
  menu_.draw("Redes por perto", items, scanCount_ + 1, wifiScanSel_, "BOOT sel | PWR nav/volta");
}

// ---------------------------------------------------------------------
// Gravacao
// ---------------------------------------------------------------------

void App::startRecording() {
  if (NoteFiles::recordingBytes() < bytesPerSec() * 2) {
    showSyncResult("Memoria insuficiente para gravar. Libere espaco nas notas ja copiadas.");
    return;
  }
  RtcDateTime now;
  if (!rtc_.getDateTime(now)) now = {2026, 1, 1, 0, 0, 0};
  notes_.buildPath(now, currentRecordingPath_, sizeof(currentRecordingPath_));

  uint32_t sampleRate = settingsStore_.get().audioSampleRateHz;
  if (!codec_.setSampleRate(sampleRate)) {
    Serial.println("!! ERRO: setSampleRate falhou.");
    return;
  }
  codec_.enable(true);
  codec_.setMicGain(settingsStore_.get().micGainDb);
  if (!recorder_.start(currentRecordingPath_, sampleRate, &codec_)) {
    codec_.enable(false);
    Serial.println("!! ERRO: Recorder.start falhou.");
    showSyncResult("Nao foi possivel iniciar gravacao; nenhum audio existente foi sobrescrito.");
    return;
  }

  recording_ = true;
  discardRecording_ = false;
  screen_ = Screen::Recording;
  recordingStartMs_ = millis();
  Screens::drawRecording(canvas_, epd_, 0, recorder_.maximumBytes() / bytesPerSec());
  Serial.printf("Gravando em %s (%lu Hz)\n", currentRecordingPath_, (unsigned long)sampleRate);
}

void App::stopRecording(bool discard) {
  if (!recording_ || screen_ == Screen::Saving) return;
  discardRecording_ = discard;
  recorder_.requestStop();
  screen_ = Screen::Saving;
  savingStartMs_ = millis(); savingLastDrawMs_ = millis() - 2000;
  markActivity();
  serviceRecordingSave();
}

void App::serviceRecordingSave() {
  // Nada de sono, rede, leitura de notas ou operacoes concorrentes enquanto
  // o writer ainda possui o PCM. O loop continua atendendo o watchdog.
  if (recorder_.isFinished()) { finishRecording(); return; }
  uint32_t now = millis();
  if (now - savingLastDrawMs_ >= 2000) {
    savingLastDrawMs_ = now;
    char message[128];
    snprintf(message, sizeof(message), "%lus persistidos\n%lu KiB escritos\nFinalizando audio...",
             (unsigned long)(recorder_.checkpointBytes() / bytesPerSec()),
             (unsigned long)(recorder_.bytesWritten() / 1024));
    Screens::drawText(canvas_, epd_, discardRecording_ ? "Descartando" : "Salvando", message, "Aguarde finalizar");
  }
}

void App::finishRecording() {
  uint32_t bytes = recorder_.finish();
  codec_.enable(false);
  recording_ = false;
  markActivity();
  Serial.printf("Gravacao parada: %u bytes, %u overflow(s) no ring buffer\n", bytes,
                (unsigned)recorder_.overflowCount());
  Serial.printf("[Record] finalize_ms=%lu checkpoint=%lu full=%d failed=%d\n",
                (unsigned long)(millis() - savingStartMs_), (unsigned long)recorder_.checkpointBytes(),
                recorder_.storageFull(), recorder_.writeFailed());
  if (discardRecording_) {
    bool removed = NoteFiles::removeWav(currentRecordingPath_); notes_.markDirty();
    if (!removed) { showSyncResult("Falha ao descartar; exclusao sera retomada ao reiniciar."); return; }
    Serial.println("Gravacao descartada."); goHome(); return;
  }

  // Nunca apresentar um marcador vazio como nota 0s. Confirmar ausencia
  // de PCM por uma abertura independente; erro de leitura nao autoriza apagar.
  File payload = NoteFiles::fs().open(NoteFiles::pcmPath(currentRecordingPath_), FILE_READ);
  bool knownEmpty = payload && !payload.isDirectory() && payload.size() == 0;
  payload.close();
  if (knownEmpty) {
    bool removed = NoteFiles::removeWav(currentRecordingPath_); notes_.markDirty();
    if (!removed) showSyncResult("Falha ao limpar gravacao vazia; limpeza sera retomada no reinicio.");
    else if (recorder_.writeFailed()) showSyncResult("Falha de gravacao: nenhuma amostra de audio foi salva.");
    else goHome();
    return;
  }

  if (recorder_.writeFailed() || (bytes && !NoteFiles::validWav(currentRecordingPath_))) {
    notes_.markDirty();
    showSyncResult("Gravacao interrompida ou falha de escrita. Audio persistido preservado; confira a nota antes de enviar.");
    return;
  }
  if (bytes != 0) {
    notes_.markDirty(); // nota nova no disco - refaz a varredura na proxima leitura
    if (settingsStore_.get().autoSyncEnabled) {
      // Com o toggle desligado (padrao), a nota fica so local ate o
      // usuario apertar "Sincronizar" - nem a transcricao roda aqui,
      // exatamente para nao depender de Wi-Fi logo apos gravar.
      startSync(-1, false, currentRecordingPath_);
      return;
    }
    if (recorder_.storageFull()) {
      showSyncResult("Memoria cheia; audio finalizado e preservado. Libere espaco antes de gravar novamente."); return;
    }
    Screens::drawSaved(canvas_, epd_, notes_.count());
    delay(1200);
  }
  goHome();
}

void App::playSelected(int index) {
  NoteEntry e;
  if (!notes_.getAt(index, e)) return;
  Serial.printf("Reproduzindo %s\n", e.path);
  if (!codec_.setVolume(settingsStore_.get().speakerVolumePercent)) {
    Serial.println("[Audio] Falha ao ajustar volume; reproducao cancelada.");
    return;
  }
  player_.play(codec_, e.path);
  codec_.enable(false);
}

void App::startAssistant() {
  if (recording_ || isSyncActive() || isExportActive() || assistantJob_.active()) return;
  free(textViewerBuf_); textViewerBuf_ = nullptr;
  NetworkRoute::configure(settingsStore_.get());
  assistantJob_.start(settingsStore_.get(), codec_, onAssistantConnectRequested_);
  screen_ = Screen::AssistantBusy;
  assistantLastDrawMs_ = millis() - 2000;
  assistantLastState_ = AssistantJob::State::Idle;
  markActivity();
  serviceAssistant();
}

void App::serviceAssistant() {
  AssistantJob::Snapshot view;
  assistantJob_.snapshot(view);
  if (view.finished) {
    String message = view.error[0] ? String(view.error) : String(view.text);
    if (!message.length()) message = view.detail;
    if (view.error[0] && view.text[0]) message += "\n\n" + String(view.text);
    if (!assistantJob_.acknowledgeFinished()) return;
    const AssistantJob::Telemetry &m = view.telemetry;
    Serial.printf("[AssistantMetrics] turn=%lu route=%s release_src=%u release_us=%lu route_begin_us=%lu route_ready_us=%lu net_connect_us=%lu ws_upgrade_us=%lu setup_complete_us=%lu capture_done_us=%lu backlog_release=%lu backlog_max=%lu input_bytes=%lu first_input_us=%lu last_input_us=%lu activity_end_us=%lu first_audio_us=%lu first_pcm_us=%lu turn_complete_us=%lu response_ready_us=%lu first_i2s_us=%lu last_i2s_us=%lu network_done_us=%lu\n",
                  static_cast<unsigned long>(m.turnId), m.routeKnown ? (m.phoneRoute ? "ble" : "wifi") : "none", m.releaseSource,
                  static_cast<unsigned long>(m.releaseUs), static_cast<unsigned long>(m.routeBeginUs),
                  static_cast<unsigned long>(m.routeReadyUs), static_cast<unsigned long>(m.networkConnectUs),
                  static_cast<unsigned long>(m.wsUpgradeUs), static_cast<unsigned long>(m.setupCompleteUs),
                  static_cast<unsigned long>(m.captureDoneUs), static_cast<unsigned long>(m.backlogAtRelease),
                  static_cast<unsigned long>(m.maxBacklogBytes), static_cast<unsigned long>(m.inputBytes),
                  static_cast<unsigned long>(m.firstInputUs), static_cast<unsigned long>(m.lastInputUs),
                  static_cast<unsigned long>(m.activityEndUs), static_cast<unsigned long>(m.firstAudioMessageUs),
                  static_cast<unsigned long>(m.firstPcmUs), static_cast<unsigned long>(m.turnCompleteUs),
                  static_cast<unsigned long>(m.responseReadyUs), static_cast<unsigned long>(m.firstI2sUs),
                  static_cast<unsigned long>(m.lastI2sUs), static_cast<unsigned long>(m.networkDoneUs));
    Serial.printf("[AssistantMedia] turn=%lu media=%s tx_bytes=%lu rx_bytes=%lu encode_us=%lu decode_us=%lu stack_free_min_bytes=%lu\n",
                  static_cast<unsigned long>(m.turnId), m.media, static_cast<unsigned long>(m.mediaSentBytes),
                  static_cast<unsigned long>(m.mediaReceivedBytes), static_cast<unsigned long>(m.encodeUs),
                  static_cast<unsigned long>(m.decodeUs), static_cast<unsigned long>(m.networkStackFreeBytes));
    Serial.printf("[AssistantDone] turn=%lu state=%d captured_ms=%lu audio_rx_ms=%lu audio_tx_ms=%lu rebuffer=%lu buffered=%d erro=%s texto=%s\n",
                  static_cast<unsigned long>(m.turnId), static_cast<int>(view.state), static_cast<unsigned long>(view.capturedMs),
                  static_cast<unsigned long>(view.receivedAudioMs), static_cast<unsigned long>(view.playedAudioMs),
                  static_cast<unsigned long>(view.rebufferCount), view.bufferedAudio,
                  view.error, view.text);
    const bool diagnosticWifi = NetworkRoute::diagnosticRoute() == DiagnosticRoute::WifiOnly;
    if (m.phoneRoute || (diagnosticWifi && PhoneRelay::ready())) NetworkRoute::finishRetainingPhone(30000);
    else NetworkRoute::finish();
    wifiMgr_.disconnect();
    screen_ = Screen::AssistantResult;
    Screens::drawText(canvas_, epd_, view.error[0] ? "Assistente: aviso" : "Assistente",
                      message.c_str(), "Segure BOOT: falar | PWR: sair");
    markActivity();
    return;
  }
  const uint32_t now = millis();
  if (view.state != assistantLastState_ || now - assistantLastDrawMs_ >= 2000) {
    String body = view.detail;
    body += "\n" + String(view.capturedMs / 1000) + " s | " + NetworkRoute::label();
    if (view.text[0]) body += "\n\n" + String(view.text);
    Screens::drawText(canvas_, epd_, "Assistente", body.c_str(),
                      view.released ? "PWR: cancelar" : "Solte BOOT para responder");
    assistantLastDrawMs_ = now;
    assistantLastState_ = view.state;
  }
}

void App::startPhonePairing() {
  if (isSyncActive() || isExportActive() || isAudioActive()) return;
  Settings &cfg = settingsStore_.get();
  if (!settingsStore_.savePhone(true, cfg.blePasskey)) {
    showSyncResult("Falha ao salvar configuracao do celular."); return;
  }
  NetworkRoute::configure(cfg);
  if (!PhoneRelay::begin(cfg.blePasskey)) {
    showSyncResult("Nao foi possivel iniciar o pareamento BLE."); return;
  }
  screen_ = Screen::PhonePairing;
  phonePairingStarted_ = millis();
  phonePairingReady_ = false;
  char message[200];
  snprintf(message, sizeof(message), "Abra o app IdeiaRec no celular e escolha Parear.\n\nPIN: %06lu\n\nJanela de 2 minutos.",
           static_cast<unsigned long>(cfg.blePasskey));
  Screens::drawText(canvas_, epd_, "Parear celular", message, "PWR/BOOT: voltar");
  markActivity();
}

void App::stopPhonePairing() { NetworkRoute::finish(); }

void App::probeAssistantModels(bool configure) {
  if (isSyncActive() || isAudioActive() || isExportActive()) return;
  const Settings &cfg = settingsStore_.get();
  if (!cfg.sttApiKey[0] || !cfg.geminiFreeConfirmed) {
    Serial.println("[Assistant] Configure a chave e confirme o projeto gratuito no portal."); return;
  }
  NetworkRoute::configure(cfg);
  if (!onAssistantConnectRequested_ || !onAssistantConnectRequested_()) {
    Serial.println("[Assistant] Sem internet para consultar modelos Live."); NetworkRoute::finish(); return;
  }
  const char *selected = nullptr;
  for (const char *candidate : {"gemini-3.8-live", "gemini-3.1-flash-live-preview"}) {
    SttNet::Response response;
    const String path = String("/v1beta/models/") + candidate;
    const bool ok = SttNet::requestJson("generativelanguage.googleapis.com", 443, "GET", path,
                                       cfg.sttApiKey, true, "", response);
    SttNet::JsonDocument doc(response);
    bool live = false;
    const cJSON *methods = doc.root ? cJSON_GetObjectItemCaseSensitive(doc.root, "supportedGenerationMethods") : nullptr;
    for (const cJSON *method = methods ? methods->child : nullptr; method; method = method->next)
      if (cJSON_IsString(method) && method->valuestring && strstr(method->valuestring, "bidiGenerateContent")) live = true;
    const bool available = ok && response.status == 200 && live;
    Serial.printf("[AssistantModel] %s HTTP=%d live=%d disponivel=%d\n", candidate, response.status, live, available);
    if (available && !selected) selected = candidate;
  }
  SttNet::endSession();
  if (configure && selected) {
    const bool saved = settingsStore_.saveAssistant(true, selected, cfg.assistantGatewayUrl,
                                                   cfg.assistantGatewayToken, cfg.assistantTimeZone);
    Serial.printf("[Assistant] modelo=%s configurado=%d\n", selected, saved);
  } else if (configure) Serial.println("[Assistant] Nenhum candidato Live disponivel; configuracao preservada.");
  NetworkRoute::finish();
  markActivity();
}

void App::startNoteExport() {
  if (isAudioActive() || isSyncActive() || isExportActive()) return;
  NoteEntry entry;
  if (!notes_.getAt(notesSel_, entry)) return;
  NetworkRoute::configure(settingsStore_.get());
  exportJob_.start(settingsStore_.get(), entry.path, onAssistantConnectRequested_);
  screen_ = Screen::ExportBusy;
  Screens::drawText(canvas_, epd_, "Obsidian", "Exportando a transcricao/Markdown para o GitHub...", "PWR: cancelar");
  markActivity();
}

void App::serviceNoteExport() {
  NoteExportJob::Snapshot view;
  exportJob_.snapshot(view);
  if (!view.finished || !exportJob_.acknowledgeFinished()) return;
  NetworkRoute::finish();
  wifiMgr_.disconnect();
  showSyncResult(view.message, true);
}

// ---------------------------------------------------------------------
// Transcricao e sincronizacao
// ---------------------------------------------------------------------

bool App::startSync(int index, bool forNote, const char *wavPath) {
  if (isSyncActive() || isExportActive() || isAudioActive()) return false;
  if (!settingsStore_.hasDriveApp() && !settingsStore_.hasDriveAuth() &&
      !settingsStore_.get().sttEndpoint[0]) {
    showSyncResult("Nenhum servico configurado. Configure STT ou Drive no menu.", forNote);
    return false;
  }
  // Lote concluido nao liga o radio apenas para descobrir a mesma fila vazia.
  if (index < 0 && !wavPath && settingsStore_.hasDriveAuth()) {
    refreshNotes();
    if (!notes_.countPendingSync(settingsStore_.get().sttEndpoint[0] != '\0', true)) {
      showSyncResult("Nenhuma nota pendente. Arquivos locais ja sincronizados.", forNote);
      return false;
    }
  }
  bool wasOff = wifiMgr_.mode() == WifiManager::Mode::Off;
  // Conexao/NTP/pareamento continuam no main, antes de capturar a configuracao
  // e escolher pendencias (um novo pareamento pode mudar a geracao do Drive).
  if (!ensureOnline()) {
    NetworkRoute::finish();
    if (wasOff) wifiMgr_.disconnect();
    reportNoNetworkAndGoHome(); return false;
  }
  if (settingsStore_.hasDriveApp() && !settingsStore_.hasDriveAuth()) {
    NetworkRoute::finish();
    if (wasOff) wifiMgr_.disconnect();
    showSyncResult("Drive nao autorizado. Conecte novamente e conclua o pareamento.", forNote);
    return false;
  }
  refreshNotes();
  free(textViewerBuf_); textViewerBuf_ = nullptr;
  SyncJob::StartResult result = syncJob_.start(notes_, index, wavPath);
  if (result != SyncJob::StartResult::Started) {
    NetworkRoute::finish();
    if (wasOff) wifiMgr_.disconnect();
    const char *message = "Nao foi possivel criar o worker de sincronizacao.";
    if (result == SyncJob::StartResult::NothingPending) message = "Nenhuma nota pendente. Arquivos locais ja sincronizados.";
    else if (result == SyncJob::StartResult::NoMemory) message = "PSRAM insuficiente para o lote.";
    showSyncResult(message, forNote); return false;
  }
  syncDisconnectAfter_ = wasOff; syncForNote_ = forNote;
  syncLastDrawMs_ = millis() - 2000;
  syncLastNote_ = 0; syncLastPercent_ = 0; syncLastPause_ = syncLastDelivered_ = false;
  screen_ = Screen::SyncBusy;
  markActivity();
  return true;
}

void App::syncOneNote(int index) { startSync(index, true); }
void App::runManualSync() { startSync(); }

void App::serviceSync() {
  SyncJob::Snapshot snapshot;
  if (!syncJob_.snapshot(snapshot)) return;
  uint32_t now = millis();
  if (snapshot.finished) {
    if (!syncJob_.acknowledgeFinished()) return;
    notes_.markDirty(); // exclusivamente main, apos worker fechar FS/TLS/NVS
    NetworkRoute::finish();
    if (syncDisconnectAfter_) wifiMgr_.disconnect();
    syncDisconnectAfter_ = false;
    String report = "IA:" + String(snapshot.ai) + " | Drive:" + String(snapshot.uploaded) +
                    " | Falhas:" + String(snapshot.failed);
    if (snapshot.paused || snapshot.pauseRequested) report += "\nPausa apos etapa.";
    if (snapshot.processedNotes < snapshot.totalNotes)
      report += "\nAinda pendentes: " + String(snapshot.totalNotes - snapshot.processedNotes);
    if (snapshot.error[0]) report += "\n" + String(snapshot.error);
    if (snapshot.warning[0]) report += "\nAviso: " + String(snapshot.warning);
    showSyncResult(report, syncForNote_);
    if (syncDiagnosticTag_[0]) {
      Serial.printf("[DiagDone] %s\n", syncDiagnosticTag_);
      syncDiagnosticTag_[0] = '\0';
    }
    return;
  }
  bool changed = snapshot.phase != syncLastPhase_ || snapshot.currentNote != syncLastNote_ ||
                 snapshot.pauseRequested != syncLastPause_ || snapshot.delivered != syncLastDelivered_;
  if (changed || now - syncLastDrawMs_ >= 2000) {
    // A copia local do modelo avanca estimativas sem concorrer com o worker.
    unsigned percent = snapshot.progress.percent(now);
    if (snapshot.currentNote != syncLastNote_) syncLastPercent_ = 0;
    if (percent < syncLastPercent_) percent = syncLastPercent_;
    syncLastPercent_ = percent;
    const char *label = snapshot.pauseRequested ? "pausa apos etapa" :
                       (snapshot.delivered ? "limpeza final" : SyncJob::phaseLabel(snapshot.phase));
    Screens::drawSyncProgress(canvas_, epd_, snapshot.noteLabel, label, percent,
                              !snapshot.delivered, snapshot.currentNote,
                              snapshot.completedNotes, snapshot.totalNotes,
                              snapshot.progress.elapsedMs(now), snapshot.progress.remainingMs(now),
                              snapshot.progress.overdue(now));
    syncLastDrawMs_ = now; syncLastPhase_ = snapshot.phase; syncLastNote_ = snapshot.currentNote;
    syncLastPause_ = snapshot.pauseRequested; syncLastDelivered_ = snapshot.delivered;
  }
}

void App::showSyncResult(const String &message, bool forNote) {
  screen_ = Screen::SyncResult;
  syncResultForNote_ = forNote;
  markActivity();
  Screens::drawText(canvas_, epd_, "Sincronizar", message.c_str(), "qualquer botao volta");
  Serial.printf("[Sync] %s\n", message.c_str());
}

void App::startWifiScanFlow() {
  Screens::drawState(canvas_, epd_, Screens::StateIcon::Wifi, "wi-fi", "escaneando redes por perto");
  int found = wifiMgr_.scan();
  const Settings &cfg = settingsStore_.get();

  scanCount_ = 0;
  for (int i = 0; i < found && scanCount_ < kMaxScanResults; i++) {
    String ssid = wifiMgr_.scanSsid(i);
    if (ssid.length() == 0) continue;

    bool alreadySaved = false;
    for (int j = 0; j < cfg.wifiNetworkCount; j++) {
      if (ssid == cfg.wifiSsid[j]) { alreadySaved = true; break; }
    }
    if (alreadySaved) continue;

    bool dup = false;
    for (int j = 0; j < scanCount_; j++) {
      if (ssid == scanResults_[j].ssid) { dup = true; break; } // roteador com 2 antenas repete SSID
    }
    if (dup) continue;

    setVal(scanResults_[scanCount_].ssid, sizeof(scanResults_[scanCount_].ssid), ssid.c_str());
    scanResults_[scanCount_].rssi = wifiMgr_.scanRssi(i);
    scanCount_++;
  }

  wifiScanSel_ = 0;
  screen_ = Screen::WifiScanList;
  drawWifiScanList();
}

void App::deleteSelectedNote() {
  if (!notes_.deleteAt(notesSel_)) {
    notes_.markDirty(); showSyncResult("Exclusao incompleta; dados preservados ou limpeza pendente de reinicio."); return;
  }
  goNotesList();
}

void App::deleteAllNotes() {
  notes_.deleteAll();
  settingsSel_ = kSetBack; // seguranca: um BOOT acidental depois disso so volta ao menu
  goSettings();
}

// ---------------------------------------------------------------------
// Botoes
// ---------------------------------------------------------------------

void App::onButton(BtnId id, BtnAction action) {
  // A soltura longa só pertence ao PTT. Não deve virar uma segunda ação nas
  // telas que já tratam pressão longa (descartar gravação, voltar, cancelar).
  if (action == BtnAction::LongPressRelease) {
    if (id == BtnId::Boot && assistantJob_.active()) assistantJob_.release(true);
    return;
  }
  markActivity();
  if (exportJob_.active()) {
    if (id == BtnId::Pwr) exportJob_.cancel();
    return;
  }
  if (assistantJob_.active()) {
    if (id == BtnId::Pwr) assistantJob_.cancel();
    return;
  }
  if (isSyncActive()) {
    if (id == BtnId::Pwr && action == BtnAction::LongPress) syncJob_.requestPause();
    return;
  }
  switch (screen_) {
    case Screen::AssistantBusy: break;
    case Screen::ExportBusy: break;
    case Screen::AssistantResult:
      if (id == BtnId::Boot && action == BtnAction::LongPress) startAssistant();
      else goHome();
      break;
    case Screen::PhonePairing:
      stopPhonePairing();
      goRootMenu();
      break;
    case Screen::Saving: break;
    case Screen::SyncBusy: break;
    case Screen::SyncResult:
      if (syncResultForNote_) goNoteDetail(notesSel_); else goHome();
      break;
    case Screen::Home: onButtonHome(id, action); break;
    case Screen::Recording: onButtonRecording(id, action); break;
    case Screen::RootMenu: onButtonRootMenu(id, action); break;
    case Screen::NotesList: onButtonNotesList(id, action); break;
    case Screen::NoteDetail: onButtonNoteDetail(id, action); break;
    case Screen::ConfirmDeleteOne: onButtonConfirmDeleteOne(id, action); break;
    case Screen::TextViewer: onButtonTextViewer(id, action); break;
    case Screen::Settings: onButtonSettings(id, action); break;
    case Screen::ConfirmDeleteAll: onButtonConfirmDeleteAll(id, action); break;
    case Screen::About: onButtonAbout(id, action); break;
    case Screen::WifiMenu: onButtonWifiMenu(id, action); break;
    case Screen::WifiNetworkDetail: onButtonWifiNetworkDetail(id, action); break;
    case Screen::WifiScanList: onButtonWifiScanList(id, action); break;
    case Screen::KeyboardSsid: onButtonKeyboardSsid(id, action); break;
    case Screen::KeyboardPassword: onButtonKeyboardPassword(id, action); break;
  }
}

void App::onButtonHome(BtnId id, BtnAction action) {
  if (id == BtnId::Boot && action == BtnAction::ShortClick) {
    startRecording();
  } else if (id == BtnId::Boot && action == BtnAction::LongPress) {
    startAssistant();
  } else if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      goRootMenu();
    } else if (action == BtnAction::LongPress) {
      if (onSleepRequested_) onSleepRequested_(); // nao retorna
    }
  }
}

void App::onButtonRecording(BtnId id, BtnAction action) {
  if (id != BtnId::Boot) return;
  if (action == BtnAction::ShortClick) {
    stopRecording();
  } else if (action == BtnAction::LongPress) {
    stopRecording(true);
  }
}

void App::onButtonRootMenu(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      rootSel_ = (rootSel_ + 1) % kRootCount;
      drawRootMenu();
    } else if (action == BtnAction::LongPress) {
      goHome();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  switch (rootSel_) {
    case kRootRecord:
      startRecording();
      break;
    case kRootNotes:
      goNotesList();
      break;
    case kRootSync:
      runManualSync();
      break;
    case kRootWifi:
      goWifiMenu();
      break;
    case kRootPhone:
      startPhonePairing();
      break;
    case kRootSettings:
      goSettings();
      break;
    case kRootAbout:
      goAbout();
      break;
  }
}

void App::onButtonNotesList(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      if (noteCount_ > 0) {
        notesSel_ = (notesSel_ + 1) % noteCount_;
        drawNotesList();
      }
    } else if (action == BtnAction::LongPress) {
      goRootMenu();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;
  if (noteCount_ == 0) {
    goRootMenu();
    return;
  }
  goNoteDetail(notesSel_);
}

void App::onButtonNoteDetail(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      noteDetailSel_ = (noteDetailSel_ + 1) % kNoteDetailCount;
      drawNoteDetail();
    } else if (action == BtnAction::LongPress) {
      goNotesList();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  switch (noteDetailSel_) {
    case kNdSummary: {
      NoteEntry e;
      if (!notes_.getAt(notesSel_, e)) break;
      if (!ensureTextViewerBuffer()) break;
      String mdPath = String(e.path);
      mdPath.replace(".wav", ".md");
      File f = LittleFS.open(mdPath, FILE_READ);
      screen_ = Screen::TextViewer;
      textViewerPage_ = 0;
      strncpy(textViewerTitle_, "Resumo", sizeof(textViewerTitle_) - 1);
      if (f) {
        size_t n = f.readBytes(textViewerBuf_, kTextViewerMaxLen - 1);
        textViewerBuf_[n] = '\0';
        f.close();
      } else {
        setVal(textViewerBuf_, kTextViewerMaxLen, "Esta nota ainda nao possui resumo gerado.");
      }
      drawTextViewer();
      break;
    }
    case kNdTranscript: {
      NoteEntry e;
      if (!notes_.getAt(notesSel_, e)) break;
      if (!ensureTextViewerBuffer()) break;
      String txtPath = String(e.path);
      txtPath.replace(".wav", ".txt");
      File f = LittleFS.open(txtPath, FILE_READ);
      screen_ = Screen::TextViewer;
      textViewerPage_ = 0;
      strncpy(textViewerTitle_, "Transcricao", sizeof(textViewerTitle_) - 1);
      if (f) {
        size_t n = f.readBytes(textViewerBuf_, kTextViewerMaxLen - 1);
        textViewerBuf_[n] = '\0';
        f.close();
      } else {
        setVal(textViewerBuf_, kTextViewerMaxLen, "Esta nota ainda nao foi transcrita.");
      }
      drawTextViewer();
      break;
    }
    case kNdPlay:
      playSelected(notesSel_);
      drawNoteDetail();
      break;
    case kNdSync:
      syncOneNote(notesSel_);
      break;
    case kNdObsidian:
      startNoteExport();
      break;
    case kNdDelete: {
      NoteEntry e;
      notes_.getAt(notesSel_, e);
      char msg[48];
      snprintf(msg, sizeof(msg), "Apagar %s ?", e.label);
      screen_ = Screen::ConfirmDeleteOne;
      Screens::drawConfirm(canvas_, epd_, "Apagar nota", msg);
      break;
    }
    case kNdBack:
      goNotesList();
      break;
  }
}

void App::onButtonConfirmDeleteOne(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    goNoteDetail(notesSel_);
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;
  deleteSelectedNote();
}

void App::onButtonTextViewer(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    free(textViewerBuf_); textViewerBuf_ = nullptr;
    screen_ = Screen::NoteDetail;
    drawNoteDetail();
    return;
  }
  if (id == BtnId::Boot && action == BtnAction::ShortClick) {
    if (textViewerTotalPages_ > 1) {
      textViewerPage_ = (textViewerPage_ + 1) % textViewerTotalPages_;
      drawTextViewer();
    }
  }
}

void App::onButtonSettings(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      settingsSel_ = (settingsSel_ + 1) % kSettingsItemCount;
      drawSettings();
    } else if (action == BtnAction::LongPress) {
      goRootMenu();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  const Settings &cfg = settingsStore_.get();
  switch (settingsSel_) {
    case kSetAudio: {
      int idx = cycleIndex(kSampleRates, kSampleRateCount, cfg.audioSampleRateHz);
      settingsStore_.saveAudio(kSampleRates[idx], cfg.micGainDb);
      break;
    }
    case kSetMicGain: {
      int idx = cycleIndex(kMicGains, kMicGainCount, cfg.micGainDb);
      settingsStore_.saveAudio(cfg.audioSampleRateHz, kMicGains[idx]);
      break;
    }
    case kSetVolume: {
      int idx = cycleIndex(kVolumes, kVolumeCount, cfg.speakerVolumePercent);
      settingsStore_.saveVolume(kVolumes[idx]);
      break;
    }
    case kSetScreensaver: {
      int idx = cycleIndex(kScreensaverTimeouts, kScreensaverTimeoutCount, cfg.screensaverTimeoutSec);
      settingsStore_.saveScreensaverTimeout(kScreensaverTimeouts[idx]);
      break;
    }
    case kSetWallpaper: {
      int8_t next = cfg.wallpaperChoice + 1;
      if (next >= WALLPAPER_COUNT) next = -1;
      settingsStore_.saveWallpaperChoice(next);
      break;
    }
    case kSetShowTemp:
      settingsStore_.saveShowTempHumidity(!cfg.showTempHumidity);
      break;
    case kSetAutoSync:
      settingsStore_.saveAutoSync(!cfg.autoSyncEnabled);
      break;
    case kSetDeleteAll: {
      confirmDeleteAllStep_ = 0;
      screen_ = Screen::ConfirmDeleteAll;
      char msg[64];
      snprintf(msg, sizeof(msg), "Apagar as %d nota(s) salvas? Nao pode ser desfeito.", noteCount_);
      Screens::drawConfirm(canvas_, epd_, "Apagar tudo?", msg);
      return; // nao redesenha settings, ja mudou de tela
    }
    case kSetBack:
      goRootMenu();
      return;
  }
  drawSettings();
}

void App::onButtonConfirmDeleteAll(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    goSettings();
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  if (confirmDeleteAllStep_ == 0) {
    confirmDeleteAllStep_ = 1;
    Screens::drawConfirm(canvas_, epd_, "Tem certeza mesmo?",
                          "Essa e a ultima chance. BOOT apaga tudo de vez.");
    return;
  }

  deleteAllNotes();
}

void App::onButtonAbout(BtnId id, BtnAction action) {
  (void)id;
  if (action != BtnAction::ShortClick && action != BtnAction::LongPress) return;
  goRootMenu();
}

void App::onButtonWifiMenu(BtnId id, BtnAction action) {
  const Settings &cfg = settingsStore_.get();
  int total = cfg.wifiNetworkCount + 4;

  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      wifiMenuSel_ = (wifiMenuSel_ + 1) % total;
      drawWifiMenu();
    } else if (action == BtnAction::LongPress) {
      goRootMenu();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  if (wifiMenuSel_ < cfg.wifiNetworkCount) {
    wifiDetailIndex_ = wifiMenuSel_;
    wifiDetailSel_ = 0;
    screen_ = Screen::WifiNetworkDetail;
    drawWifiNetworkDetail();
    return;
  }

  int actionIdx = wifiMenuSel_ - cfg.wifiNetworkCount;
  if (actionIdx == 0) {
    startWifiScanFlow();
  } else if (actionIdx == 1) {
    kbSsidBuf_[0] = '\0';
    keyboard_.begin("SSID da rede", kbSsidBuf_, sizeof(kbSsidBuf_), false);
    screen_ = Screen::KeyboardSsid;
  } else if (actionIdx == 2) {
    if (onPortalRequested_) onPortalRequested_();
    drawWifiMenu(); // screen_ continua WifiMenu; so redesenha ao voltar do portal
  } else {
    goRootMenu();
  }
}

void App::onButtonWifiNetworkDetail(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      wifiDetailSel_ = (wifiDetailSel_ + 1) % 4;
      drawWifiNetworkDetail();
    } else if (action == BtnAction::LongPress) {
      goWifiMenu();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  const Settings &cfg = settingsStore_.get();
  switch (wifiDetailSel_) {
    case 0: { // Conectar agora
      Screens::drawState(canvas_, epd_, Screens::StateIcon::Wifi, "conectando",
                          cfg.wifiSsid[wifiDetailIndex_]);
      bool ok = onConnectRequested_ ? onConnectRequested_() : false;
      Screens::drawText(canvas_, epd_, "Wi-Fi", ok ? "Conectado!" : "Nao foi possivel conectar.",
                         "qualquer botao volta");
      break;
    }
    case 1: { // Marcar/tirar favorita
      bool isFavorite = strcmp(cfg.wifiSsid[wifiDetailIndex_], cfg.favoriteWifiSsid) == 0;
      settingsStore_.saveFavoriteWifi(isFavorite ? "" : cfg.wifiSsid[wifiDetailIndex_]);
      drawWifiNetworkDetail();
      break;
    }
    case 2: // Remover
      settingsStore_.removeWifiNetwork(wifiDetailIndex_);
      goWifiMenu();
      break;
    case 3: // Voltar
      goWifiMenu();
      break;
  }
}

void App::onButtonWifiScanList(BtnId id, BtnAction action) {
  if (scanCount_ == 0) {
    if (id == BtnId::Pwr) goWifiMenu();
    return;
  }

  int total = scanCount_ + 1;
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) {
      wifiScanSel_ = (wifiScanSel_ + 1) % total;
      drawWifiScanList();
    } else if (action == BtnAction::LongPress) {
      goWifiMenu();
    }
    return;
  }
  if (id != BtnId::Boot || action != BtnAction::ShortClick) return;

  if (wifiScanSel_ == scanCount_) {
    goWifiMenu();
    return;
  }

  setVal(kbSsidBuf_, sizeof(kbSsidBuf_), scanResults_[wifiScanSel_].ssid);
  kbPassBuf_[0] = '\0';
  char title[48];
  snprintf(title, sizeof(title), "Senha de %s", kbSsidBuf_);
  keyboard_.begin(title, kbPassBuf_, sizeof(kbPassBuf_), true);
  screen_ = Screen::KeyboardPassword;
}

void App::onButtonKeyboardSsid(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) keyboard_.onNext();
    else if (action == BtnAction::LongPress) keyboard_.onNextRow();
  } else if (id == BtnId::Boot && action == BtnAction::ShortClick) {
    keyboard_.onSelect();
  }

  if (!keyboard_.isDone()) return;
  if (!keyboard_.wasConfirmed() || kbSsidBuf_[0] == '\0') {
    goWifiMenu();
    return;
  }

  kbPassBuf_[0] = '\0';
  char title[48];
  snprintf(title, sizeof(title), "Senha de %s", kbSsidBuf_);
  keyboard_.begin(title, kbPassBuf_, sizeof(kbPassBuf_), true);
  screen_ = Screen::KeyboardPassword;
}

void App::onButtonKeyboardPassword(BtnId id, BtnAction action) {
  if (id == BtnId::Pwr) {
    if (action == BtnAction::ShortClick) keyboard_.onNext();
    else if (action == BtnAction::LongPress) keyboard_.onNextRow();
  } else if (id == BtnId::Boot && action == BtnAction::ShortClick) {
    keyboard_.onSelect();
  }

  if (!keyboard_.isDone()) return;
  if (keyboard_.wasConfirmed()) {
    settingsStore_.saveWifiNetwork(kbSsidBuf_, kbPassBuf_);
  }
  goWifiMenu();
}

// ---------------------------------------------------------------------
// Loop principal
// ---------------------------------------------------------------------

void App::loop() {
  NetworkRoute::service();
  if (exportJob_.active() || screen_ == Screen::ExportBusy) { serviceNoteExport(); return; }
  if (assistantJob_.active() || screen_ == Screen::AssistantBusy) { serviceAssistant(); return; }
  if (screen_ == Screen::PhonePairing) {
    const bool ready = PhoneRelay::ready();
    if (ready && !phonePairingReady_) {
      phonePairingReady_ = true;
      Screens::drawText(canvas_, epd_, "Celular conectado", "Pareamento pronto. O app pode trabalhar em segundo plano."
                        "\n\nAperte um botao para voltar.", "PWR/BOOT: voltar");
    }
    if (millis() - phonePairingStarted_ >= 120000) { stopPhonePairing(); goHome(); }
    return;
  }
  if (isSyncActive()) { serviceSync(); return; }
  if (screen_ == Screen::Saving) { serviceRecordingSave(); return; }
  if (screen_ == Screen::Recording) {
    static uint32_t lastUiUpdate = 0;
    uint32_t now = millis();
    if (now - lastUiUpdate >= 1000) {
      lastUiUpdate = now;
      uint32_t elapsedSec = (now - recordingStartMs_) / 1000;
      uint32_t written = recorder_.bytesWritten();
      uint32_t freeSec = (recorder_.maximumBytes() - written) / bytesPerSec();
      Screens::drawRecording(canvas_, epd_, elapsedSec, freeSec);
      if (recorder_.overflowCount() > 0) {
        Serial.printf("!! AVISO: %lu overflow(s) no ring buffer de audio\n",
                      (unsigned long)recorder_.overflowCount());
      }
    }
    if (recorder_.storageFull() || recorder_.writeFailed()) {
      stopRecording();
    }
    return;
  }

  // Fora da gravacao, qualquer tela (inicial ou menus) dorme sozinha
  // depois do tempo configurado sem apertar botao - o objetivo e nao
  // gastar bateria com a tela acesa mesmo se o usuario for embora no
  // meio de um menu.
  uint32_t timeoutMs = settingsStore_.get().screensaverTimeoutSec * 1000UL;
  if (!diagnosticAwake_ && millis() - lastActivityMs_ >= timeoutMs) {
    if (onSleepRequested_) onSleepRequested_(); // nao retorna
  }
}

void App::diagnosticCommand(const String &command) {
  DiagnosticDone done{command};
  markActivity();
  if (exportJob_.active()) {
    Serial.println("[Diag] exportacao ao Obsidian em andamento"); return;
  }
  if (command == "benchmark-route auto" || command == "benchmark-route wifi" || command == "benchmark-route phone") {
    if (assistantJob_.active() || isSyncActive()) {
      Serial.println("[Diag] rota de benchmark nao pode mudar durante uma operacao"); return;
    }
    const auto route = command == "benchmark-route wifi" ? DiagnosticRoute::WifiOnly :
      command == "benchmark-route phone" ? DiagnosticRoute::PhoneOnly : DiagnosticRoute::Auto;
    NetworkRoute::setDiagnosticRoute(route);
    Serial.printf("[BenchmarkRoute] %s\n", route == DiagnosticRoute::WifiOnly ? "wifi" :
      route == DiagnosticRoute::PhoneOnly ? "phone" : "auto");
    return;
  }
  if (command == "benchmark-playback full" || command == "benchmark-playback ready") {
    if (assistantJob_.active() || isSyncActive()) {
      Serial.println("[Diag] buffer de benchmark nao pode mudar durante uma operacao"); return;
    }
    const bool early = command == "benchmark-playback ready";
    NetworkRoute::setDiagnosticEarlyPlayback(early);
    Serial.printf("[BenchmarkPlayback] %s\n", early ? "ready" : "full");
    return;
  }
  if (command.startsWith("benchmark-media ")) {
    if (assistantJob_.active() || isSyncActive()) {
      Serial.println("[Diag] midia nao pode mudar durante uma operacao"); return;
    }
    const String name = command.substring(16);
    DiagnosticMedia profile;
    if (name == "default") profile = NetworkRoute::kDefaultPhoneMedia;
    else if (name == "legacy") profile = DiagnosticMedia::Legacy;
    else if (name == "pcm") profile = DiagnosticMedia::Pcm;
    else if (name == "opus64") profile = DiagnosticMedia::Opus64;
    else if (name == "opus48") profile = DiagnosticMedia::Opus48;
    else if (name == "opus32") profile = DiagnosticMedia::Opus32;
    else { Serial.println("[Diag] midia invalida; use default/legacy/pcm/opus64/opus48/opus32"); return; }
    NetworkRoute::setDiagnosticMedia(profile);
    Serial.printf("[BenchmarkMedia] %s\n", name.c_str());
    return;
  }
  if (command == "assistant-models" || command == "assistant-enable") {
    probeAssistantModels(command == "assistant-enable"); return;
  }
  if (command == "phone-pair" && !isSyncActive() && !isAudioActive()) { startPhonePairing(); return; }
  if (command == "phone-stop" && screen_ == Screen::PhonePairing) { stopPhonePairing(); goHome(); return; }
  if (command == "assistant-start" && !isSyncActive() && !isAudioActive()) { startAssistant(); return; }
  if (command == "assistant-release") { assistantJob_.release(); return; }
  if (command == "assistant-cancel") { assistantJob_.cancel(); return; }
  if (assistantJob_.active()) {
    AssistantJob::Snapshot snapshot;
    assistantJob_.snapshot(snapshot);
    Serial.printf("[Assistant] turn=%lu state=%d captured_ms=%lu released=%d finished=%d route=%s\n",
                  static_cast<unsigned long>(snapshot.telemetry.turnId), static_cast<int>(snapshot.state), static_cast<unsigned long>(snapshot.capturedMs),
                  snapshot.released, snapshot.finished, NetworkRoute::label());
    if (command == "status") Serial.println("[DiagDone] status");
    return;
  }
  if (recording_) {
    Serial.printf("[Record] busy=1 saving=%d bytes=%lu checkpoint=%lu overflow=%lu full=%d failed=%d\n",
                  screen_ == Screen::Saving, (unsigned long)recorder_.bytesWritten(),
                  (unsigned long)recorder_.checkpointBytes(), (unsigned long)recorder_.overflowCount(),
                  recorder_.storageFull(), recorder_.writeFailed());
    if (command == "status") Serial.println("[DiagDone] status");
    return;
  }
  if (isSyncActive()) {
    SyncJob::Snapshot snapshot;
    if (command == "status" && syncJob_.snapshot(snapshot)) {
      uint32_t now = millis();
      Serial.printf("[Status] busy=1 nota=%s fase=%s progresso=%s%u%% atual=%u concluidas=%u total=%u IA=%u Drive=%u falhas=%u elapsed_ms=%lu remaining_ms=%lu overdue=%d pausa=%d\n",
                    snapshot.noteLabel, SyncJob::phaseLabel(snapshot.phase),
                    snapshot.delivered ? "" : "~", snapshot.progress.percent(now),
                    snapshot.currentNote, snapshot.completedNotes, snapshot.totalNotes,
                    snapshot.ai, snapshot.uploaded, snapshot.failed,
                    (unsigned long)snapshot.progress.elapsedMs(now),
                    (unsigned long)snapshot.progress.remainingMs(now),
                    snapshot.progress.overdue(now), snapshot.pauseRequested);
      Serial.printf("[Status] STT=%s MD=%s HTTP=%lu TLS=%lu erro=%s\n",
                    snapshot.transcriptModel, snapshot.markdownModel,
                    (unsigned long)snapshot.counters.httpRequests,
                    (unsigned long)snapshot.counters.tlsHandshakes, snapshot.error);
    } else {
      Serial.println("[Diag] busy: sincronizacao ativa; use status ou PWR longo para pausar apos etapa");
    }
    return;
  }
  if (command == "status") {
    const Settings &cfg = settingsStore_.get();
    Serial.printf("[Status] notas=%d pendentes=%d livres=%llu gravaveis=%llu psram=%u\n",
                  notes_.count(), notes_.countPendingSync(cfg.sttEndpoint[0], settingsStore_.hasDriveAuth()),
                  (unsigned long long)NoteFiles::freeBytes(), (unsigned long long)NoteFiles::recordingBytes(),
                  ESP.getFreePsram());
    Serial.printf("[Status] IA auto=%d gratuito_confirmado=%d STT=%s MD=%s Drive_config=%d autorizado=%d\n",
                  cfg.sttAutoModel, cfg.geminiFreeConfirmed, cfg.sttModel, cfg.summaryModel,
                  settingsStore_.hasDriveApp(), settingsStore_.hasDriveAuth());
    Serial.printf("[Status] rota=%s BLE_config=%d BLE_ativo=%d BLE_pronto=%d assistente=%d live=%s gateway=%d heap=%u\n",
                  NetworkRoute::label(), cfg.bleFallbackEnabled, PhoneRelay::active(), PhoneRelay::ready(),
                  cfg.assistantEnabled, cfg.assistantModel, cfg.assistantGatewayUrl[0] != '\0', ESP.getFreeHeap());
    Serial.printf("[Status] BLE_erro=%s\n", PhoneRelay::lastError());
    const PhoneRelay::Diagnostics ble = PhoneRelay::diagnostics();
    Serial.printf("[BLE] ligado=%d autenticado=%d subscribed=%d mtu=%u interval_1.25ms=%u latency=%u supervision_10ms=%u tx_phy=%u rx_phy=%u tx_octets=%u rx_octets=%u tx_time=%u rx_time=%u security_rc=%d bond_rc=%d disconnect_reason=%d auth_flags=%d pin_requests=%lu\n",
                  ble.linked, ble.authenticated, ble.subscribed, ble.mtu, ble.interval1250us, ble.latency,
                  ble.supervisionTimeout10ms, ble.txPhy, ble.rxPhy, ble.maxTxOctets, ble.maxRxOctets,
                  ble.maxTxTime, ble.maxRxTime, ble.securityResult, ble.bondResult,
                  ble.disconnectReason, ble.authenticationFlags, static_cast<unsigned long>(ble.passkeyRequests));
    for (int i = 0; i < notes_.count(); ++i) {
      NoteEntry note; if (!notes_.getAt(i, note)) continue;
      NoteSyncState state; GDriveClient::getNoteSyncState(note.path, state);
      Serial.printf("[Note] %s bytes=%u txt=%d md=%d synced=%d wav=%d txtup=%d mdup=%d HTTP=%d erro=%s\n",
                    note.path, note.sizeBytes, note.hasTxt, note.hasMd, state.fullySynced,
                    state.wavUploaded, state.txtUploaded, state.mdUploaded,
                    state.lastStatusCode, state.lastError);
    }
  } else if (!recording_ && command == "battery") {
    Serial.printf("[Battery] percent=%d\n", Battery::readPercent());
  } else if (command == "stayawake 1") {
    diagnosticAwake_ = true; Serial.println("[Diag] descanso suspenso durante testes USB");
  } else if (command == "stayawake 0") {
    diagnosticAwake_ = false; Serial.println("[Diag] descanso normal restaurado");
  } else if (!recording_ && command == "sync") {
    runManualSync();
    if (isSyncActive()) {
      done.enabled = false; setVal(syncDiagnosticTag_, sizeof(syncDiagnosticTag_), "sync");
    }
  } else if (!recording_ && command == "sync-one") {
    syncOneNote(0);
    if (isSyncActive()) {
      done.enabled = false; setVal(syncDiagnosticTag_, sizeof(syncDiagnosticTag_), "sync-one");
    }
  } else if (!recording_ && command == "wifi") {
    ensureOnline();
  } else if (!recording_ && command == "repair-drive") {
    if (!settingsStore_.saveDriveRefreshToken("")) {
      Serial.println("[Diag] Falha ao invalidar autorizacao do Drive"); return;
    }
    notes_.markDirty();
    NetworkRoute::finish();
    wifiMgr_.disconnect();
    bool connected = ensureOnline();
    markActivity();
    Serial.printf("[Diag] Pareamento Drive: %s\n", connected && settingsStore_.hasDriveAuth() ? "autorizado" : "pendente");
  } else {
    Serial.println("[Diag] comandos: status | battery | stayawake 1/0 | wifi | sync | sync-one | repair-drive | benchmark-route auto/wifi/phone | benchmark-playback full/ready");
  }
}
