#pragma once
#include "../display/epaper.h"
#include "../ui/canvas.h"
#include "../ui/menu.h"
#include "../ui/keyboard.h"
#include "../board/rtc.h"
#include "../board/buttons.h"
#include "../audio/codec.h"
#include "../audio/recorder.h"
#include "../audio/player.h"
#include "../storage/notes.h"
#include "../net/settings.h"
#include "../net/wifi_mgr.h"
#include "../net/stt.h"
#include "../net/gdrive.h"

// Maquina de estados de toda a UI interativa: tela inicial, gravacao,
// menus, notas (com detalhe/apagar) e configuracoes. Tudo que roda so
// uma vez no boot (Wi-Fi, pareamento do Drive, NTP) e o caminho do sono
// (tela de bloqueio, deep sleep) ficam fora - continuam em main.cpp,
// que mexe direto com esp_sleep e nao faz parte deste loop de botoes.
class App {
public:
  App(EPaperDisplay &epd, Canvas &canvas, NotesStore &notes, SettingsStore &settingsStore,
      AudioCodec &codec, Recorder &recorder, Player &player, WifiManager &wifiMgr,
      SttClient &sttClient, GDriveClient &gdrive, Rtc &rtc);

  // onSleepRequested e chamado quando o app decide dormir (timeout de
  // inatividade na tela inicial, ou PWR longo na tela inicial). Nao
  // retorna (entra em deep sleep) - ver enterScreensaver() em main.cpp.
  using SleepRequestFn = void (*)();
  // onConnectRequested liga o radio, escaneia e conecta na rede salva
  // mais forte por perto (bloqueante); retorna se conseguiu. Ver
  // onConnectRequested() em main.cpp (tambem sincroniza NTP e checa
  // pareamento do Drive quando conecta).
  using ConnectRequestFn = bool (*)();
  // onPortalRequested sobe o Access Point + portal web e bloqueia ate o
  // usuario sair (BOOT longo) ou salvar (reinicia o ESP). Usado para
  // configurar STT/Drive, que nao cabem no teclado de 2 botoes.
  using PortalRequestFn = void (*)();
  void begin(SleepRequestFn onSleepRequested, ConnectRequestFn onConnectRequested,
             PortalRequestFn onPortalRequested);

  void onButton(BtnId id, BtnAction action);

  // Chamar toda iteracao do loop() principal: atualiza a tela de
  // gravacao a cada segundo e verifica o timeout de inatividade.
  void loop();

private:
  enum class Screen {
    Home,
    Recording,
    RootMenu,
    NotesList,
    NoteDetail,
    ConfirmDeleteOne,
    TranscriptView,
    Settings,
    ConfirmDeleteAll,
    About,
    WifiMenu,
    WifiNetworkDetail,
    WifiScanList,
    KeyboardSsid,
    KeyboardPassword,
  };

  EPaperDisplay &epd_;
  Canvas &canvas_;
  NotesStore &notes_;
  SettingsStore &settingsStore_;
  AudioCodec &codec_;
  Recorder &recorder_;
  Player &player_;
  WifiManager &wifiMgr_;
  SttClient &sttClient_;
  GDriveClient &gdrive_;
  Rtc &rtc_;
  Menu menu_;
  Keyboard keyboard_;

  Screen screen_ = Screen::Home;
  SleepRequestFn onSleepRequested_ = nullptr;
  ConnectRequestFn onConnectRequested_ = nullptr;
  PortalRequestFn onPortalRequested_ = nullptr;

  int noteCount_ = 0;
  int rootSel_ = 0;
  int notesSel_ = 0;
  int noteDetailSel_ = 0;
  int settingsSel_ = 0;
  int confirmDeleteAllStep_ = 0;

  int wifiMenuSel_ = 0;
  int wifiDetailIndex_ = -1;
  int wifiDetailSel_ = 0;
  int wifiScanSel_ = 0;
  static constexpr int kMaxScanResults = 12;
  struct ScanResult {
    char ssid[33];
    int rssi;
  };
  ScanResult scanResults_[kMaxScanResults];
  int scanCount_ = 0;
  char kbSsidBuf_[33] = "";
  char kbPassBuf_[65] = "";

  bool recording_ = false;
  char currentRecordingPath_[48] = "";
  uint32_t recordingStartMs_ = 0;
  char lastTxtPath_[48] = "";

  uint32_t lastActivityMs_ = 0;

  void markActivity();
  uint32_t bytesPerSec() const;
  void formatDuration(uint32_t bytes, uint32_t sampleRateHz, char *out, size_t outLen) const;
  void refreshNotes();

  void goHome();
  void goRootMenu();
  void goNotesList();
  void goNoteDetail(int index);
  void goSettings();
  void goAbout();
  void goWifiMenu();

  void drawHome();
  void drawRootMenu();
  void drawNotesList();
  void drawNoteDetail();
  void drawSettings();
  void drawWifiMenu();
  void drawWifiNetworkDetail();
  void drawWifiScanList();

  void startRecording();
  void stopRecording();
  void playSelected(int index);

  void transcribeIfPossible(const char *wavPath);
  void syncIfPossible(const char *wavPath);
  bool transcribeNote(const char *wavPath);
  void runManualSync();
  void syncOneNote(int index);
  void deleteSelectedNote();
  void deleteAllNotes();
  bool ensureOnline(); // conecta sob demanda se ainda nao estiver (bloqueante)
  void reportNoNetworkAndGoHome();
  void startWifiScanFlow();

  void onButtonHome(BtnId id, BtnAction action);
  void onButtonRecording(BtnId id, BtnAction action);
  void onButtonRootMenu(BtnId id, BtnAction action);
  void onButtonNotesList(BtnId id, BtnAction action);
  void onButtonNoteDetail(BtnId id, BtnAction action);
  void onButtonConfirmDeleteOne(BtnId id, BtnAction action);
  void onButtonTranscriptView(BtnId id, BtnAction action);
  void onButtonSettings(BtnId id, BtnAction action);
  void onButtonConfirmDeleteAll(BtnId id, BtnAction action);
  void onButtonAbout(BtnId id, BtnAction action);
  void onButtonWifiMenu(BtnId id, BtnAction action);
  void onButtonWifiNetworkDetail(BtnId id, BtnAction action);
  void onButtonWifiScanList(BtnId id, BtnAction action);
  void onButtonKeyboardSsid(BtnId id, BtnAction action);
  void onButtonKeyboardPassword(BtnId id, BtnAction action);
};
