#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <esp_sleep.h>

#include "config/pins.h"
#include "board/power.h"
#include "board/buttons.h"
#include "board/rtc.h"
#include "display/epaper.h"
#include "ui/canvas.h"
#include "ui/wallpapers.h"
#include "audio/codec.h"
#include "audio/recorder.h"
#include "audio/player.h"
#include "storage/notes.h"
#include "net/settings.h"
#include "net/wifi_mgr.h"
#include "net/stt.h"
#include "net/gdrive.h"

static BoardPower power(PIN_EPD_PWR, PIN_AUDIO_PWR, PIN_VBAT_PWR);
static Rtc rtc;
static Buttons buttons;
static AudioCodec codec;
static Recorder recorder;
static Player player;
static NotesStore notes;
static SettingsStore settingsStore;
static WifiManager wifiMgr;
static SttClient sttClient;
static GDriveClient gdrive;
static char lastTxtPath[48] = "";
static uint32_t lastActivityMs = 0;
static void markActivity() { lastActivityMs = millis(); }

// Taxa vem das configuracoes (ajustavel pelo menu) - PCM 16-bit mono,
// entao bytes/s = taxa * 2. Notas antigas gravadas numa taxa diferente
// mostram a duracao calculada com a taxa ATUAL, nao a que foi usada na
// hora (limitacao conhecida - o WAV em si guarda a taxa certa).
static uint32_t bytesPerSec() { return settingsStore.get().audioSampleRateHz * 2; }

enum class AppState { Idle, Recording, Playing };
static AppState appState = AppState::Idle;

static int selectedIndex = 0;
static int noteCount = 0;
static char currentRecordingPath[48];
static uint32_t recordingStartMs = 0;

static EPaperPins epdPins = {
  .cs = PIN_EPD_CS, .dc = PIN_EPD_DC, .rst = PIN_EPD_RST, .busy = PIN_EPD_BUSY,
  .mosi = PIN_EPD_MOSI, .sck = PIN_EPD_SCK, .spiHost = SPI2_HOST
};
static EPaperDisplay epd(200, 200, epdPins);
static Canvas canvas(epd);

static void formatDuration(uint32_t bytes, uint32_t sampleRateHz, char *out, size_t outLen) {
  uint32_t bps = sampleRateHz > 0 ? sampleRateHz * 2 : bytesPerSec();
  float secs = bytes / (float)bps;
  snprintf(out, outLen, "%.0fs", secs);
}

static void drawIdleScreen() {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 6, "Minhas notas", EPD_BLACK, 1);

  uint64_t freeB = notes.freeBytes();
  uint32_t freeSecs = (uint32_t)(freeB / bytesPerSec());
  char freeLine[32];
  snprintf(freeLine, sizeof(freeLine), "livre: ~%um%02us", freeSecs / 60, freeSecs % 60);
  canvas.drawText(8, 16, freeLine, EPD_BLACK, 1);

  String wifiLine = wifiMgr.isConnected() ? ("wifi: " + wifiMgr.statusLine())
                                           : ("config: " + wifiMgr.statusLine());
  canvas.drawText(8, 26, wifiLine.c_str(), EPD_BLACK, 1);
  canvas.drawFastHLine(4, 36, 192, EPD_BLACK);

  if (noteCount == 0) {
    canvas.drawText(8, 44, "Nenhuma nota ainda.", EPD_BLACK, 1);
    canvas.drawText(8, 56, "Aperte BOOT p/ gravar.", EPD_BLACK, 1);
  } else {
    int visible = min(noteCount, 12);
    for (int i = 0; i < visible; i++) {
      NoteEntry e;
      if (!notes.getAt(i, e)) continue;
      char durBuf[8];
      formatDuration(e.sizeBytes, e.sampleRateHz, durBuf, sizeof(durBuf));

      char line[32];
      snprintf(line, sizeof(line), "%c %s %s", i == selectedIndex ? '>' : ' ', e.label, durBuf);
      canvas.drawText(8, 42 + i * 11, line, EPD_BLACK, 1);
    }
  }

  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "BOOT grava | PWR nav/toca", EPD_BLACK, 1);
  epd.displayPart();
}

static void drawRecordingScreen(uint32_t elapsedSec, uint32_t freeSec) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 8, "Gravando...", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 20, 192, EPD_BLACK);

  char line[24];
  snprintf(line, sizeof(line), "%um%02us", elapsedSec / 60, elapsedSec % 60);
  canvas.drawText(60, 80, line, EPD_BLACK, 2);

  char freeLine[32];
  snprintf(freeLine, sizeof(freeLine), "espaco p/ mais %um%02us", freeSec / 60, freeSec % 60);
  canvas.drawText(8, 130, freeLine, EPD_BLACK, 1);

  canvas.drawText(8, 184, "BOOT para parar", EPD_BLACK, 1);
  epd.displayPart();
}

static void drawWifiSetupScreen(const String &line1, const String &line2) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 8, "Configurar Wi-Fi", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 20, 192, EPD_BLACK);
  canvas.drawWrappedText(8, 34, line1.c_str(), EPD_BLACK, 1, 30, 10);
  canvas.drawWrappedText(8, 90, line2.c_str(), EPD_BLACK, 1, 30, 10);
  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "BOOT longo: usar offline", EPD_BLACK, 1);
  epd.displayPart();
}

static bool g_skipWifiSetup = false;
static void onWifiSetupButtonEvent(BtnId id, BtnAction action) {
  if (id == BtnId::Boot && action == BtnAction::LongPress) {
    g_skipWifiSetup = true;
  }
}

// Roda so no boot: tenta conectar ao Wi-Fi salvo; se precisar do portal
// AP, fica nessa tela ate o usuario configurar pelo celular (o proprio
// portal reinicia o ESP quando salva) ou apertar BOOT longo para pular
// e usar o app offline.
static void runWifiSetup() {
  buttons.setCallback(onWifiSetupButtonEvent);

  if (wifiMgr.begin(settingsStore)) {
    return; // conectado direto com credenciais salvas
  }

  String line1 = "Rede: " + wifiMgr.apName();
  String line2 = "Acesse http://" + WiFi.softAPIP().toString() + " pelo celular";
  drawWifiSetupScreen(line1, line2);

  while (!g_skipWifiSetup) {
    wifiMgr.loop();
    buttons.poll();
    delay(5);
  }
  Serial.println("Configuracao de Wi-Fi pulada - modo offline.");
}

static void drawDrivePairingScreen(const char *userCode, const char *verificationUrl) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 8, "Autorizar Google Drive", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 20, 192, EPD_BLACK);
  canvas.drawText(8, 34, "Acesse:", EPD_BLACK, 1);
  canvas.drawText(8, 46, verificationUrl, EPD_BLACK, 1);
  canvas.drawText(8, 66, "Digite o codigo:", EPD_BLACK, 1);
  canvas.drawText(8, 82, userCode, EPD_BLACK, 2);
  epd.displayPart();
}

// So roda quando o usuario ja configurou Client ID/Secret pelo portal
// mas ainda nao autorizou (sem refresh_token salvo) - mostra o codigo
// de pareamento e espera o usuario aprovar em outro aparelho.
static void runDrivePairingIfNeeded() {
  if (!wifiMgr.isConnected()) return;
  if (!settingsStore.hasDriveApp() || settingsStore.hasDriveAuth()) return;

  if (gdrive.pairDevice(settingsStore, drawDrivePairingScreen)) {
    canvas.clear(EPD_WHITE);
    canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
    canvas.drawText(8, 90, "Google Drive", EPD_BLACK, 1);
    canvas.drawText(8, 102, "autorizado!", EPD_BLACK, 1);
    epd.displayPart();
    delay(2000);
  }
}

static void refreshNotes() {
  noteCount = notes.count();
  if (selectedIndex >= noteCount) selectedIndex = max(0, noteCount - 1);
}

static void startRecording() {
  RtcDateTime now;
  if (!rtc.getDateTime(now)) {
    now = {2026, 1, 1, 0, 0, 0};
  }
  notes.buildPath(now, currentRecordingPath, sizeof(currentRecordingPath));

  uint32_t sampleRate = settingsStore.get().audioSampleRateHz;
  if (!codec.setSampleRate(sampleRate)) {
    Serial.println("!! ERRO: setSampleRate falhou.");
    return;
  }
  codec.enable(true);
  codec.setMicGain(settingsStore.get().micGainDb);
  if (!recorder.start(currentRecordingPath, sampleRate, &codec)) {
    Serial.println("!! ERRO: Recorder.start falhou.");
    return;
  }
  appState = AppState::Recording;
  recordingStartMs = millis();
  drawRecordingScreen(0, (uint32_t)(notes.freeBytes() / bytesPerSec()));
  Serial.printf("Gravando em %s (%lu Hz)\n", currentRecordingPath, (unsigned long)sampleRate);
}

static void drawTranscribingScreen() {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 8, "Transcrevendo...", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 20, 192, EPD_BLACK);
  canvas.drawText(8, 90, "enviando audio para", EPD_BLACK, 1);
  canvas.drawText(8, 102, "o servico de STT", EPD_BLACK, 1);
  epd.displayPart();
}

static void drawTranscriptScreen(const char *text) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 8, "Nota transcrita:", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 20, 192, EPD_BLACK);
  canvas.drawWrappedText(8, 28, text, EPD_BLACK, 1, 30, 10);
  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "BOOT grava | PWR nav/toca", EPD_BLACK, 1);
  epd.displayPart();
}

static void drawSyncingScreen() {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 8, "Sincronizando...", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 20, 192, EPD_BLACK);
  canvas.drawText(8, 90, "enviando para o", EPD_BLACK, 1);
  canvas.drawText(8, 102, "Google Drive", EPD_BLACK, 1);
  epd.displayPart();
}

// Se houver Wi-Fi e um endpoint de STT configurado, transcreve o WAV e
// salva o .txt ao lado dele. Silenciosamente pulado sem rede/config -
// a nota continua salva e reproduzivel normalmente.
static void transcribeIfPossible(const char *wavPath) {
  lastTxtPath[0] = '\0';

  const Settings &cfg = settingsStore.get();
  if (!wifiMgr.isConnected() || cfg.sttEndpoint[0] == '\0') return;

  drawTranscribingScreen();

  static char textBuf[SttClient::kMaxTextLen];
  if (!sttClient.transcribe(cfg, wavPath, textBuf, sizeof(textBuf))) {
    Serial.println("Transcricao falhou, nota continua salva sem .txt.");
    return;
  }

  String txtPath = String(wavPath);
  txtPath.replace(".wav", ".txt");
  File f = LittleFS.open(txtPath, FILE_WRITE);
  if (f) {
    f.print(textBuf);
    f.close();
    strncpy(lastTxtPath, txtPath.c_str(), sizeof(lastTxtPath) - 1);
  }

  drawTranscriptScreen(textBuf);
  delay(3000); // da tempo de ler antes de eventualmente voltar pra lista
}

// Se houver Wi-Fi, sincronizacao automatica ligada e o dispositivo ja
// pareado com o Drive, sobe o .wav (+ .txt, se a transcricao rolou).
// Silenciosamente pulado sem rede/pareamento - a nota fica so local.
static void syncIfPossible(const char *wavPath) {
  const Settings &cfg = settingsStore.get();
  if (!wifiMgr.isConnected() || !cfg.autoSyncEnabled || !settingsStore.hasDriveAuth()) return;

  drawSyncingScreen();
  const char *txtPath = lastTxtPath[0] ? lastTxtPath : nullptr;
  if (!gdrive.uploadNote(settingsStore, wavPath, txtPath)) {
    Serial.println("Sincronizacao falhou, nota continua so local.");
  }
}

static void stopRecording() {
  uint32_t bytes = recorder.stop();
  codec.enable(false);
  appState = AppState::Idle;
  Serial.printf("Gravacao parada: %u bytes\n", bytes);

  if (bytes == 0) {
    LittleFS.remove(currentRecordingPath);
  } else {
    transcribeIfPossible(currentRecordingPath);
    syncIfPossible(currentRecordingPath);
  }
  refreshNotes();
  selectedIndex = 0; // nota mais nova = topo da lista
  drawIdleScreen();
}

static void playSelected() {
  NoteEntry e;
  if (!notes.getAt(selectedIndex, e)) return;

  Serial.printf("Reproduzindo %s\n", e.path);
  appState = AppState::Playing;
  codec.setVolume(20.0f);
  player.play(codec, e.path);
  codec.enable(false);
  appState = AppState::Idle;
  drawIdleScreen();
}

// Mostra um wallpaper aleatorio com refresh completo (sem ghosting) e
// desliga a alimentacao do e-paper - o painel e biestavel, entao a
// imagem fica visivel sem energia. Acorda so com BOOT (GPIO0), que
// reinicia o ESP32 do zero (deep sleep nao preserva RAM).
static void enterScreensaver() {
  Serial.println("Entrando em modo de descanso (deep sleep)...");

  int idx = random(WALLPAPER_COUNT);
  epd.init(); // recarrega a LUT de refresh completo (o app roda em modo parcial)
  memcpy_P(epd.buffer(), WALLPAPERS[idx], epd.bufferLen());
  epd.display();
  power.epdOff();

  codec.enable(false);
  power.audioOff();

  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_BOOT, 0);
  esp_deep_sleep_start();
}

static void onButtonEvent(BtnId id, BtnAction action) {
  markActivity();

  if (id == BtnId::Boot && action == BtnAction::ShortClick) {
    if (appState == AppState::Idle) {
      startRecording();
    } else if (appState == AppState::Recording) {
      stopRecording();
    }
  } else if (id == BtnId::Pwr && appState == AppState::Idle) {
    if (action == BtnAction::ShortClick) {
      if (noteCount > 0) {
        selectedIndex = (selectedIndex + 1) % noteCount;
        drawIdleScreen();
      }
    } else if (action == BtnAction::LongPress) {
      if (noteCount > 0) playSelected();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=================================");
  Serial.println("Fase 3 - Notas e UI");
  Serial.println("=================================");

  if (!LittleFS.begin(true)) {
    Serial.println("!! ERRO: LittleFS.begin() falhou.");
  }
  notes.begin();

  power.epdOn();
  power.audioOn();
  delay(10);

  if (!rtc.begin(PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("!! AVISO: RTC PCF85063 nao respondeu no I2C.");
  }

  epd.init();
  epd.clear();
  epd.displayPartBaseImage();
  epd.initPartial();

  if (!codec.begin()) {
    Serial.println("!! ERRO: AudioCodec.begin() falhou.");
  }

  buttons.begin(PIN_BTN_BOOT, PIN_BTN_PWR, onButtonEvent);

  settingsStore.begin();
  runWifiSetup();
  runDrivePairingIfNeeded();
  buttons.setCallback(onButtonEvent);

  refreshNotes();
  drawIdleScreen();
  markActivity();
  Serial.printf("%d nota(s) encontradas. Pronto.\n", noteCount);
}

void loop() {
  buttons.poll();
  wifiMgr.loop();

  if (appState == AppState::Recording) {
    // A captura roda em tasks proprias (ver audio/recorder.cpp) - o
    // loop() so cuida da UI, sem competir pelo tempo do I2S.
    static uint32_t lastUiUpdate = 0;
    uint32_t now = millis();
    if (now - lastUiUpdate >= 1000) {
      lastUiUpdate = now;
      uint32_t elapsedSec = (now - recordingStartMs) / 1000;
      uint32_t freeSec = (uint32_t)(notes.freeBytes() / bytesPerSec());
      drawRecordingScreen(elapsedSec, freeSec);
      if (recorder.overflowCount() > 0) {
        Serial.printf("!! AVISO: %lu overflow(s) no ring buffer de audio\n",
                      (unsigned long)recorder.overflowCount());
      }
    }
  } else if (appState == AppState::Idle) {
    uint32_t timeoutMs = settingsStore.get().screensaverTimeoutSec * 1000UL;
    if (millis() - lastActivityMs >= timeoutMs) {
      enterScreensaver(); // nao retorna - entra em deep sleep
    }
  }
}
