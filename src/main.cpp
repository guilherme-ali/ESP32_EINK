#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <esp_sleep.h>

#include "config/pins.h"
#include "board/power.h"
#include "board/buttons.h"
#include "board/rtc.h"
#include "board/battery.h"
#include "board/shtc3.h"
#include "display/epaper.h"
#include "ui/canvas.h"
#include "ui/wallpapers.h"
#include "ui/lockscreen.h"
#include "audio/codec.h"
#include "audio/recorder.h"
#include "audio/player.h"
#include "storage/notes.h"
#include "net/settings.h"
#include "net/wifi_mgr.h"
#include "net/stt.h"
#include "net/gdrive.h"
#include "app/app.h"

static BoardPower power(PIN_EPD_PWR, PIN_AUDIO_PWR, PIN_VBAT_PWR);
static Rtc rtc;
static Shtc3 shtc3;
static Buttons buttons;
static AudioCodec codec;
static Recorder recorder;
static Player player;
static NotesStore notes;
static SettingsStore settingsStore;
static WifiManager wifiMgr;
static SttClient sttClient;
static GDriveClient gdrive;

static EPaperPins epdPins = {
  .cs = PIN_EPD_CS, .dc = PIN_EPD_DC, .rst = PIN_EPD_RST, .busy = PIN_EPD_BUSY,
  .mosi = PIN_EPD_MOSI, .sck = PIN_EPD_SCK, .spiHost = SPI2_HOST
};
static EPaperDisplay epd(200, 200, epdPins);
static Canvas canvas(epd);
static LockScreen lockScreen(epd, canvas);

// A UI interativa inteira (tela inicial, gravacao, menus, notas,
// configuracoes) vive na maquina de estados de App - main.cpp so cuida
// do que roda uma vez no boot (Wi-Fi, pareamento do Drive, NTP) e do
// caminho do sono (tela de bloqueio, deep sleep), que mexem direto com
// esp_sleep e ficam fora do loop de botoes do App.
static App app(epd, canvas, notes, settingsStore, codec, recorder, player, wifiMgr, sttClient,
               gdrive, rtc);

// Sobrevive ao deep sleep (RTC memory) - o wallpaper da tela de
// descanso fica o mesmo durante todo um periodo de descanso, so muda
// quando um novo periodo comeca (o usuario mexeu no aparelho e ele
// voltou a ficar ocioso - ver markActivity()).
RTC_DATA_ATTR static int rtcWallpaperIndex = -1;

static void markActivity() { rtcWallpaperIndex = -1; }

static void onButtonEvent(BtnId id, BtnAction action) {
  markActivity();
  app.onButton(id, action);
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

// O PCF85063 nao tem como saber a hora certa sozinho - so acerta com
// ajuda de fora. Sem isso, o relogio fica preso em qualquer data que
// tenha sido gravada nele uma vez (foi o que aconteceu: meses de testes
// com uma data de fabrica/teste nunca corrigida). Roda uma vez, toda
// vez que conecta no Wi-Fi - barato e mantem o relogio sempre certo.
static void syncRtcFromNtp() {
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 8000)) {
    Serial.println("!! AVISO: NTP nao respondeu, RTC continua com a hora antiga.");
    return;
  }

  RtcDateTime dt;
  dt.year = timeinfo.tm_year + 1900;
  dt.month = timeinfo.tm_mon + 1;
  dt.day = timeinfo.tm_mday;
  dt.hour = timeinfo.tm_hour;
  dt.minute = timeinfo.tm_min;
  dt.second = timeinfo.tm_sec;
  rtc.setDateTime(dt);
  Serial.printf("RTC sincronizado via NTP: %04u-%02u-%02u %02u:%02u:%02u\n", dt.year, dt.month,
                dt.day, dt.hour, dt.minute, dt.second);
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

// Callback que o App chama sob demanda (Sincronizar, menu Wi-Fi) pra
// ligar o radio, escanear e conectar na rede salva mais forte por
// perto. Aproveita a conexao pra tambem acertar o relogio via NTP e
// checar o pareamento do Drive, do jeito que era feito so no boot antes.
static bool onConnectRequested() {
  bool ok = wifiMgr.connect(settingsStore);
  if (ok) {
    syncRtcFromNtp();
    runDrivePairingIfNeeded();
  }
  return ok;
}

// Callback que o App chama a partir do menu Wi-Fi ("Portal") pra
// configurar STT/Drive (campos longos demais pro teclado de 2 botoes)
// ou cadastrar Wi-Fi pelo celular. Sobe o Access Point, mostra a tela
// de instrucoes e bloqueia ate BOOT longo (ou ate o portal salvar, que
// reinicia o ESP sozinho).
static void onPortalRequested() {
  g_skipWifiSetup = false;
  buttons.setCallback(onWifiSetupButtonEvent);
  wifiMgr.startApPortal(settingsStore);

  String line1 = "Rede: " + wifiMgr.apName();
  String line2 = "Acesse http://" + WiFi.softAPIP().toString() + " pelo celular";
  drawWifiSetupScreen(line1, line2);

  while (!g_skipWifiSetup) {
    wifiMgr.loop();
    buttons.poll();
    delay(5);
  }

  wifiMgr.disconnect();
  buttons.setCallback(onButtonEvent);
  Serial.println("Portal de configuracao fechado.");
}

// Roda so no boot. No uso normal o radio fica desligado (WIFI_OFF) e so
// liga sob demanda (ver onConnectRequested/onPortalRequested acima) -
// o portal automatico aqui e so pro PRIMEIRO uso, quando ainda nao ha
// nenhuma rede nem STT/Drive configurados (sem isso nao haveria como
// entrar com nada pela primeira vez).
static void runWifiSetup() {
  const Settings &cfg = settingsStore.get();
  bool needsInitialSetup =
      cfg.wifiNetworkCount == 0 && cfg.sttEndpoint[0] == '\0' && !settingsStore.hasDriveApp();

  if (!needsInitialSetup) {
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi desligado - conecta sob demanda (Sincronizar ou menu Wi-Fi).");
    return;
  }

  Serial.println("Primeiro uso: nenhuma rede/config encontrada, abrindo portal.");
  onPortalRequested();
}

// Monta o LockScreenStatus com o que der pra ler rapido (RTC + bateria
// + SHTC3, todos ja com o barramento I2C ligado pelo chamador).
static LockScreenStatus buildLockStatus() {
  LockScreenStatus status = {};

  RtcDateTime now;
  status.hasTime = rtc.getDateTime(now);
  if (status.hasTime) status.time = now;

  power.vbatOn();
  delay(5); // divisor resistivo estabilizar
  status.batteryPercent = Battery::readPercent();
  power.vbatOff();

  status.hasTempHumidity = settingsStore.get().showTempHumidity &&
                            shtc3.read(status.tempC, status.humidity);

  status.pendingSyncCount = notes.countPendingSync();
  return status;
}

// Desenha a tela de bloqueio (wallpaper sorteado uma vez por periodo de
// descanso + cartao com hora/bateria/sensores) e desliga a alimentacao
// do e-paper - o painel e biestavel, entao a imagem fica visivel sem
// energia. Dois jeitos de acordar: os botoes (ext1, retoma o app) ou um
// timer curto (so redesenha o relogio e volta a dormir, ver
// runLockScreenRefreshAndSleep() em setup()).
static void enterScreensaver() {
  Serial.println("Entrando em modo de descanso (deep sleep)...");

  if (rtcWallpaperIndex < 0) {
    rtcWallpaperIndex = settingsStore.get().wallpaperChoice >= 0
                             ? settingsStore.get().wallpaperChoice % WALLPAPER_COUNT
                             : random(WALLPAPER_COUNT);
  }

  epd.init(); // recarrega a LUT de refresh completo (o app roda em modo parcial)
  lockScreen.draw(rtcWallpaperIndex, buildLockStatus());
  power.epdOff();

  codec.enable(false);
  power.audioOff();

  esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_PWR),
                                ESP_EXT1_WAKEUP_ANY_LOW);
  uint32_t refreshSec = settingsStore.get().lockRefreshSec;
  if (refreshSec > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)refreshSec * 1000000ULL);
  }
  esp_deep_sleep_start();
}

// So roda quando o wake foi por timer (relogio da tela de bloqueio) -
// religa o minimo (I2C + e-paper), redesenha e volta a dormir sem tocar
// em codec/Wi-Fi/botoes. Nunca retorna.
static void runLockScreenRefreshAndSleep() {
  power.epdOn();
  delay(10);
  rtc.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  shtc3.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  settingsStore.begin();
  LittleFS.begin(true);
  notes.begin();

  epd.init();
  lockScreen.draw(rtcWallpaperIndex, buildLockStatus());
  power.epdOff();

  esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_PWR),
                                ESP_EXT1_WAKEUP_ANY_LOW);
  uint32_t refreshSec = settingsStore.get().lockRefreshSec;
  if (refreshSec > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)refreshSec * 1000000ULL);
  }
  esp_deep_sleep_start();
}

void setup() {
  // Wake por timer (relogio da tela de bloqueio, a cada poucos minutos)
  // usa um caminho bem mais curto - so I2C+e-paper, sem WiFi/codec/
  // botoes - pra gastar o minimo de bateria possivel. Precisa ser a
  // primeira coisa no setup(), antes de qualquer init pesado.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    runLockScreenRefreshAndSleep(); // nao retorna
  }

  Serial.begin(115200);
  delay(2000);
  Serial.println("=================================");
  Serial.println("Fase 4 - Menus e arquivos");
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
  shtc3.begin(PIN_I2C_SDA, PIN_I2C_SCL);

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

  app.begin(enterScreensaver, onConnectRequested, onPortalRequested);
  Serial.printf("%d nota(s) encontradas. Pronto.\n", notes.count());
}

void loop() {
  buttons.poll();
  wifiMgr.loop();
  app.loop();
}
