#include "app.h"
#include "../ui/screens.h"
#include "../ui/wallpapers.h"
#include "../board/battery.h"
#include <LittleFS.h>

namespace {

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

enum RootIdx { kRootRecord = 0, kRootNotes, kRootSync, kRootWifi, kRootSettings, kRootAbout, kRootCount };

enum NoteDetailIdx { kNdPlay = 0, kNdTranscript, kNdSync, kNdDelete, kNdBack, kNoteDetailCount };

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
      gdrive_(gdrive), rtc_(rtc), shtc3_(shtc3), menu_(canvas, epd), keyboard_(canvas, epd) {}

void App::begin(SleepRequestFn onSleepRequested, ConnectRequestFn onConnectRequested,
                 PortalRequestFn onPortalRequested) {
  onSleepRequested_ = onSleepRequested;
  onConnectRequested_ = onConnectRequested;
  onPortalRequested_ = onPortalRequested;
  refreshNotes();
  screen_ = Screen::Home;
  drawHome();
  markActivity();
}

void App::markActivity() { lastActivityMs_ = millis(); }

uint32_t App::bytesPerSec() const { return settingsStore_.get().audioSampleRateHz * 2; }

void App::formatDuration(uint32_t bytes, uint32_t sampleRateHz, char *out, size_t outLen) const {
  uint32_t bps = sampleRateHz > 0 ? sampleRateHz * 2 : bytesPerSec();
  float secs = bytes / (float)bps;
  snprintf(out, outLen, "%.0fs", secs);
}

void App::refreshNotes() {
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
  if (wifiMgr_.isConnected()) return true;
  Screens::drawState(canvas_, epd_, Screens::StateIcon::Wifi, "conectando",
                      "procurando redes salvas por perto");
  return onConnectRequested_ ? onConnectRequested_() : false;
}

// Nenhuma rede salva estava por perto - avisa e volta pra tela inicial
// sozinho (em vez de deixar o usuario preso numa tela de sincronizar
// que nao vai a lugar nenhum sem Wi-Fi).
void App::reportNoNetworkAndGoHome() {
  Screens::drawText(canvas_, epd_, "Sincronizar",
                     "Nenhuma rede salva por perto. Aproxime de uma rede conhecida ou "
                     "cadastre uma nova no menu Wi-Fi.");
  delay(2500);
  goHome();
}

// ---------------------------------------------------------------------
// Desenho de cada tela
// ---------------------------------------------------------------------

void App::drawHome() {
  RtcDateTime now;
  bool timeValid = rtc_.getDateTime(now) && now.year >= 2024;
  int pending = notes_.countPendingSync();
  int batteryPercent = Battery::readPercent();

  float tempC = 0, humidity = 0;
  bool hasTempHumidity = shtc3_.read(tempC, humidity);

  Screens::drawHome(canvas_, epd_, now, timeValid, batteryPercent,
                     settingsStore_.get().showTempHumidity, hasTempHumidity, tempC, humidity,
                     noteCount_, pending);
}

void App::drawRootMenu() {
  int pending = notes_.countPendingSync();
  MenuItem items[kRootCount];
  setItem(items[kRootRecord], "Gravar");
  setItem(items[kRootNotes], "Notas");
  snprintf(items[kRootNotes].value, sizeof(items[kRootNotes].value), "%d", noteCount_);
  setItem(items[kRootSync], "Sincronizar");
  if (pending > 0) snprintf(items[kRootSync].value, sizeof(items[kRootSync].value), "%d", pending);
  setItem(items[kRootWifi], "Wi-Fi");
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
  setItem(items[kNdPlay], "Reproduzir");
  setItem(items[kNdTranscript], "Ver transcricao");
  setVal(items[kNdTranscript].value, sizeof(items[kNdTranscript].value), e.hasTxt ? "ok" : "sem");
  setItem(items[kNdSync], "Sincronizar esta");
  setVal(items[kNdSync].value, sizeof(items[kNdSync].value), e.hasSnc ? "enviada" : "pendente");
  setItem(items[kNdDelete], "Apagar");
  setItem(items[kNdBack], "Voltar");

  menu_.draw(e.label, items, kNoteDetailCount, noteDetailSel_, "BOOT sel | PWR nav/volta");
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
  snprintf(items[kSetVolume].value, sizeof(items[kSetVolume].value), "%d", (int)cfg.speakerVolumeDb);

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
    Serial.println("!! ERRO: Recorder.start falhou.");
    return;
  }

  recording_ = true;
  screen_ = Screen::Recording;
  recordingStartMs_ = millis();
  Screens::drawRecording(canvas_, epd_, 0, (uint32_t)(notes_.freeBytes() / bytesPerSec()));
  Serial.printf("Gravando em %s (%lu Hz)\n", currentRecordingPath_, (unsigned long)sampleRate);
}

void App::stopRecording() {
  uint32_t bytes = recorder_.stop();
  codec_.enable(false);
  recording_ = false;
  Serial.printf("Gravacao parada: %u bytes, %u overflow(s) no ring buffer\n", bytes,
                (unsigned)recorder_.overflowCount());

  if (bytes == 0) {
    LittleFS.remove(currentRecordingPath_);
  } else {
    notes_.markDirty(); // nota nova no disco - refaz a varredura na proxima leitura
    if (settingsStore_.get().autoSyncEnabled) {
      // Com o toggle desligado (padrao), a nota fica so local ate o
      // usuario apertar "Sincronizar" - nem a transcricao roda aqui,
      // exatamente para nao depender de Wi-Fi logo apos gravar.
      transcribeIfPossible(currentRecordingPath_);
      syncIfPossible(currentRecordingPath_);
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
  codec_.setVolume(settingsStore_.get().speakerVolumeDb);
  player_.play(codec_, e.path);
  codec_.enable(false);
}

// ---------------------------------------------------------------------
// Transcricao e sincronizacao
// ---------------------------------------------------------------------

bool App::transcribeNote(const char *wavPath) {
  const Settings &cfg = settingsStore_.get();
  if (cfg.sttEndpoint[0] == '\0') return false;

  static char textBuf[SttClient::kMaxTextLen];
  if (!sttClient_.transcribe(cfg, wavPath, textBuf, sizeof(textBuf))) return false;

  String txtPath = String(wavPath);
  txtPath.replace(".wav", ".txt");
  File f = LittleFS.open(txtPath, FILE_WRITE);
  if (!f) return false;
  f.print(textBuf);
  f.close();
  return true;
}

// So roda logo apos gravar (se autoSyncEnabled) - o resto das notas fica
// pendente ate o usuario apertar "Sincronizar" no menu (runManualSync()).
void App::transcribeIfPossible(const char *wavPath) {
  lastTxtPath_[0] = '\0';
  const Settings &cfg = settingsStore_.get();
  if (!wifiMgr_.isConnected() || cfg.sttEndpoint[0] == '\0') return;

  Screens::drawState(canvas_, epd_, Screens::StateIcon::Activity, "transcrevendo",
                      "enviando audio para o servico de STT");

  static char textBuf[SttClient::kMaxTextLen];
  if (!sttClient_.transcribe(cfg, wavPath, textBuf, sizeof(textBuf))) {
    Serial.println("Transcricao falhou, nota continua salva sem .txt.");
    return;
  }

  String txtPath = String(wavPath);
  txtPath.replace(".wav", ".txt");
  File f = LittleFS.open(txtPath, FILE_WRITE);
  if (f) {
    f.print(textBuf);
    f.close();
    strncpy(lastTxtPath_, txtPath.c_str(), sizeof(lastTxtPath_) - 1);
    notes_.markDirty();
  }

  Screens::drawText(canvas_, epd_, "Nota transcrita:", textBuf, "BOOT grava | PWR menu");
  delay(3000); // da tempo de ler antes de seguir pra tela inicial
}

void App::syncIfPossible(const char *wavPath) {
  const Settings &cfg = settingsStore_.get();
  if (!wifiMgr_.isConnected() || !cfg.autoSyncEnabled || !settingsStore_.hasDriveAuth()) return;

  Screens::drawState(canvas_, epd_, Screens::StateIcon::Activity, "sincronizando",
                      "enviando para o Google Drive");
  const char *txtPath = lastTxtPath_[0] ? lastTxtPath_ : nullptr;
  if (!gdrive_.uploadNote(settingsStore_, wavPath, txtPath)) {
    Serial.println("Sincronizacao falhou, nota continua so local.");
  }
  notes_.markDirty(); // uploadNote() cria o .snc quando da certo
}

void App::syncOneNote(int index) {
  NoteEntry e;
  if (!notes_.getAt(index, e)) return;

  bool wasOffline = !wifiMgr_.isConnected();
  if (!ensureOnline()) {
    reportNoNetworkAndGoHome();
    return;
  }

  Screens::drawState(canvas_, epd_, Screens::StateIcon::Activity, "sincronizando", e.label);

  String txtPath = String(e.path);
  txtPath.replace(".wav", ".txt");
  if (!LittleFS.exists(txtPath) && settingsStore_.get().sttEndpoint[0] != '\0') {
    transcribeNote(e.path);
  }
  bool hasTxt = LittleFS.exists(txtPath);
  bool ok = settingsStore_.hasDriveAuth() &&
            gdrive_.uploadNote(settingsStore_, e.path, hasTxt ? txtPath.c_str() : nullptr);
  notes_.markDirty();

  if (wasOffline) wifiMgr_.disconnect();

  Screens::drawText(canvas_, epd_, "Sincronizar", ok ? "Nota enviada." : "Falha ao enviar.",
                     "qualquer botao volta");
}

// Varre /notes procurando o que falta transcrever (sem .txt) ou subir
// (sem .snc) e resolve tudo numa passada so, com progresso na tela.
void App::runManualSync() {
  bool wasOffline = !wifiMgr_.isConnected();
  if (!ensureOnline()) {
    reportNoNetworkAndGoHome();
    return;
  }

  refreshNotes();
  int total = noteCount_;

  int pendingTotal = 0;
  for (int i = 0; i < total; i++) {
    NoteEntry e;
    if (!notes_.getAt(i, e)) continue;
    String txtPath = String(e.path); txtPath.replace(".wav", ".txt");
    String sncPath = String(e.path); sncPath.replace(".wav", ".snc");
    bool needsTxt = settingsStore_.get().sttEndpoint[0] != '\0' && !LittleFS.exists(txtPath);
    bool needsUpload = settingsStore_.hasDriveAuth() && !LittleFS.exists(sncPath);
    if (needsTxt || needsUpload) pendingTotal++;
  }

  if (pendingTotal == 0) {
    if (wasOffline) wifiMgr_.disconnect();
    Screens::drawSyncSummary(canvas_, epd_, 0, 0, 0, 0);
    return;
  }

  int transcribed = 0, uploaded = 0, failed = 0, done = 0;
  for (int i = 0; i < total; i++) {
    NoteEntry e;
    if (!notes_.getAt(i, e)) continue;
    String txtPath = String(e.path); txtPath.replace(".wav", ".txt");
    String sncPath = String(e.path); sncPath.replace(".wav", ".snc");
    bool needsTxt = settingsStore_.get().sttEndpoint[0] != '\0' && !LittleFS.exists(txtPath);
    bool needsUpload = settingsStore_.hasDriveAuth() && !LittleFS.exists(sncPath);
    if (!needsTxt && !needsUpload) continue;

    done++;
    Screens::drawState(canvas_, epd_, Screens::StateIcon::Activity, "sincronizando", e.label,
                        done, pendingTotal);

    if (needsTxt && transcribeNote(e.path)) transcribed++;
    if (needsUpload) {
      bool hasTxt = LittleFS.exists(txtPath);
      if (gdrive_.uploadNote(settingsStore_, e.path, hasTxt ? txtPath.c_str() : nullptr)) {
        uploaded++;
      } else {
        failed++;
      }
    }
  }

  notes_.markDirty();
  if (wasOffline) wifiMgr_.disconnect();
  Screens::drawSyncSummary(canvas_, epd_, transcribed, uploaded, failed, pendingTotal);
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
  notes_.deleteAt(notesSel_);
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
  markActivity();
  switch (screen_) {
    case Screen::Home: onButtonHome(id, action); break;
    case Screen::Recording: onButtonRecording(id, action); break;
    case Screen::RootMenu: onButtonRootMenu(id, action); break;
    case Screen::NotesList: onButtonNotesList(id, action); break;
    case Screen::NoteDetail: onButtonNoteDetail(id, action); break;
    case Screen::ConfirmDeleteOne: onButtonConfirmDeleteOne(id, action); break;
    case Screen::TranscriptView: onButtonTranscriptView(id, action); break;
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
    recorder_.stop();
    codec_.enable(false);
    recording_ = false;
    LittleFS.remove(currentRecordingPath_);
    Serial.println("Gravacao descartada.");
    goHome();
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
    case kNdPlay:
      playSelected(notesSel_);
      drawNoteDetail();
      break;
    case kNdTranscript: {
      NoteEntry e;
      if (!notes_.getAt(notesSel_, e)) break;
      String txtPath = String(e.path);
      txtPath.replace(".wav", ".txt");
      File f = LittleFS.open(txtPath, FILE_READ);
      screen_ = Screen::TranscriptView;
      if (f) {
        static char buf[SttClient::kMaxTextLen];
        size_t n = f.readBytes(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        f.close();
        Screens::drawText(canvas_, epd_, "Transcricao", buf, "PWR volta");
      } else {
        Screens::drawText(canvas_, epd_, "Transcricao", "Esta nota ainda nao foi transcrita.",
                           "PWR volta");
      }
      break;
    }
    case kNdSync:
      syncOneNote(notesSel_);
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

void App::onButtonTranscriptView(BtnId id, BtnAction action) {
  (void)id;
  if (action != BtnAction::ShortClick && action != BtnAction::LongPress) return;
  screen_ = Screen::NoteDetail;
  drawNoteDetail();
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
      int idx = cycleIndex(kVolumes, kVolumeCount, cfg.speakerVolumeDb);
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
  if (screen_ == Screen::Recording) {
    static uint32_t lastUiUpdate = 0;
    uint32_t now = millis();
    if (now - lastUiUpdate >= 1000) {
      lastUiUpdate = now;
      uint32_t elapsedSec = (now - recordingStartMs_) / 1000;
      uint32_t freeSec = (uint32_t)(notes_.freeBytes() / bytesPerSec());
      Screens::drawRecording(canvas_, epd_, elapsedSec, freeSec);
      if (recorder_.overflowCount() > 0) {
        Serial.printf("!! AVISO: %lu overflow(s) no ring buffer de audio\n",
                      (unsigned long)recorder_.overflowCount());
      }
    }
    return;
  }

  // Fora da gravacao, qualquer tela (inicial ou menus) dorme sozinha
  // depois do tempo configurado sem apertar botao - o objetivo e nao
  // gastar bateria com a tela acesa mesmo se o usuario for embora no
  // meio de um menu.
  uint32_t timeoutMs = settingsStore_.get().screensaverTimeoutSec * 1000UL;
  if (millis() - lastActivityMs_ >= timeoutMs) {
    if (onSleepRequested_) onSleepRequested_(); // nao retorna
  }
}
