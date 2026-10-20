#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <driver/rtc_io.h>
#include <sys/time.h>
#include <time.h>

#include "config/pins.h"
#include "board/power.h"
#include "board/buttons.h"
#include "board/rtc.h"
#include "board/battery.h"
#include "board/shtc3.h"
#include "board/boot_trace.h"
#include "board/sleep_gate.h"
#include "board/wake_intent.h"
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
#include "connectivity/network_route.h"

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
               gdrive, rtc, shtc3);
static bool sleepPending = false;
static bool sleepDeferred = false;
static bool uiStarted = false;
static bool buttonQueueFailed = false;
static SleepGate sleepGate;
static TaskHandle_t buttonTask = nullptr;
RTC_DATA_ATTR static uint32_t rtcSleepEntries = 0;

static void printBootMetrics() {
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("[Boot] reset_reason=%d wakeup=%d ext1_mask=0x%llx sleep_entries=%lu\n",
                (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
                (unsigned long long)esp_sleep_get_ext1_wakeup_status(), (unsigned long)rtcSleepEntries);
  const bool displayBusy = app.feedbackBusy();
  Serial.printf("[PowerState] codec_quiescent=%d epd_ready=%d epd_healthy=%d can_sleep=%d display_busy=%d\n",
                codec.quiescent(), displayBusy ? -1 : epd.ready(), displayBusy ? -1 : epd.healthy(),
                app.canSleep(), displayBusy);
  Serial.printf("[BootMetrics] entry_us=%lu button_observed_us=%lu settings_ready_us=%lu audio_ready_us=%lu first_block_us=%lu fs_ready_us=%lu intent_us=%lu checkpoint_us=%lu display_ready_us=%lu ui_ready_us=%lu valid_audio_us=unmeasured heap_internal=%u largest_internal=%u psram_free=%u\n",
                (unsigned long)BootTrace::get(BootTrace::Entry),
                (unsigned long)BootTrace::get(BootTrace::ButtonObserved),
                (unsigned long)BootTrace::get(BootTrace::SettingsReady),
                (unsigned long)BootTrace::get(BootTrace::AudioReady),
                (unsigned long)BootTrace::get(BootTrace::FirstBlock),
                (unsigned long)BootTrace::get(BootTrace::FilesystemReady),
                (unsigned long)BootTrace::get(BootTrace::IntentResolved),
                (unsigned long)BootTrace::get(BootTrace::FirstCheckpoint),
                (unsigned long)BootTrace::get(BootTrace::DisplayReady),
                (unsigned long)BootTrace::get(BootTrace::UiReady),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
}

static bool prepareUi() {
  if (uiStarted) return epd.healthy();
  uiStarted = true;
  shtc3.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  bool ok = epd.ready(); // The audio-view worker may already have initialized it.
  if (!ok && epd.healthy()) {
    power.epdOn();
    delay(10);
    ok = epd.init();
    if (ok) { epd.clear(); ok = epd.displayPartBaseImage() && epd.initPartial(); }
  }
  if (ok) BootTrace::mark(BootTrace::DisplayReady);
  else { power.epdOff(); epd.invalidatePower(); Serial.println("[Display] Painel indisponivel; audio continua independente."); }
  return ok;
}

static void powerOnFeedbackDisplay() {
  power.epdOn();
  delay(10);
}

static void sampleButtons(void *) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    buttons.sample();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(5));
  }
}

// Sobrevive ao deep sleep (RTC memory) - o wallpaper da tela de
// descanso fica o mesmo durante todo um periodo de descanso, so muda
// quando um novo periodo comeca (o usuario mexeu no aparelho e ele
// voltou a ficar ocioso - ver markActivity()).
RTC_DATA_ATTR static int rtcWallpaperIndex = -1;

static void markActivity() { rtcWallpaperIndex = -1; }

static void onButtonEvent(BtnId id, BtnAction action) {
  if (sleepPending || buttonQueueFailed) return;
  if (action == BtnAction::PressDown) sleepDeferred = false;
  markActivity();
  const uint32_t dispatchedMs = millis();
  const uint32_t observedMs = buttons.lastEventObservedMs();
  app.onButton(id, action);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("[Button] id=%u action=%u observed_ms=%lu dispatched_ms=%lu max_gap_ms=%lu gaps=%lu\n",
                (unsigned)id, (unsigned)action, (unsigned long)observedMs,
                (unsigned long)dispatchedMs, (unsigned long)buttons.maxSampleGapMs(),
                (unsigned long)buttons.sampleGapCount());
#else
  (void)dispatchedMs;
  (void)observedMs;
#endif
}

static void drawWifiSetupScreen(const String &line1, const String &line2) {
  canvas.clear(EPD_WHITE);
  canvas.drawText(8, 8, "configurar wi-fi", EPD_BLACK, FONT_EMPHASIS);
  canvas.drawFastHLine(8, 30, 184, EPD_BLACK);
  canvas.drawWrappedText(8, 42, line1.c_str(), EPD_BLACK, FONT_BODY, 184, 18);
  canvas.drawWrappedText(8, 96, line2.c_str(), EPD_BLACK, FONT_BODY, 184, 18);
  canvas.drawFastHLine(8, 178, 184, EPD_BLACK);
  canvas.drawText(8, 181, "BOOT longo: usar offline", EPD_BLACK, FONT_BODY);
  epd.displayPart();
}

static bool g_skipWifiSetup = false;
static bool g_cancelDrivePairing = false;
static bool g_ntpPending = false;
static uint32_t g_ntpPollAt = 0;
static void onWifiSetupButtonEvent(BtnId id, BtnAction action) {
  if (id == BtnId::Boot && action == BtnAction::LongPress) {
    g_skipWifiSetup = true;
  }
}

static void onDrivePairingButton(BtnId id, BtnAction action) {
  if (action == BtnAction::LongPress || (id == BtnId::Pwr && action == BtnAction::ShortClick)) {
    g_cancelDrivePairing = true;
  }
}

static bool serviceDrivePairing() {
  buttons.poll();
  wifiMgr.loop();
  return !g_cancelDrivePairing;
}

// O PCF85063 nao tem como saber a hora certa sozinho - so acerta com
// ajuda de fora. O NTP e iniciado ao conectar, sem bloquear a sincronizacao;
// serviceNtpRtc grava o RTC quando a hora de sistema estiver disponivel.
static void syncRtcFromNtp() {
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");
  g_ntpPending = true;
  g_ntpPollAt = 0;
}
static void serviceNtpRtc() {
  if (!g_ntpPending || millis() - g_ntpPollAt < 1000) return;
  g_ntpPollAt = millis();
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1)) return;

  RtcDateTime dt;
  dt.year = timeinfo.tm_year + 1900;
  dt.month = timeinfo.tm_mon + 1;
  dt.day = timeinfo.tm_mday;
  dt.hour = timeinfo.tm_hour;
  dt.minute = timeinfo.tm_min;
  dt.second = timeinfo.tm_sec;
  if (!rtc.setDateTime(dt)) return;
  g_ntpPending = false;
  Serial.printf("RTC sincronizado via NTP: %04u-%02u-%02u %02u:%02u:%02u\n", dt.year, dt.month,
                dt.day, dt.hour, dt.minute, dt.second);
}

static void drawDrivePairingScreen(const char *userCode, const char *verificationUrl) {
  Serial.printf("[Drive] Autorize em %s com o codigo %s\n", verificationUrl, userCode);
  canvas.clear(EPD_WHITE);
  canvas.drawText(8, 8, "autorizar google drive", EPD_BLACK, FONT_EMPHASIS);
  canvas.drawFastHLine(8, 30, 184, EPD_BLACK);
  canvas.drawText(8, 42, "Acesse:", EPD_BLACK, FONT_BODY);
  canvas.drawText(8, 56, verificationUrl, EPD_BLACK, FONT_BODY);
  canvas.drawText(8, 78, "Digite o codigo:", EPD_BLACK, FONT_BODY);
  canvas.drawText(8, 94, userCode, EPD_BLACK, FONT_EMPHASIS);
  epd.displayPart();
}

// So roda quando o usuario ja configurou Client ID/Secret pelo portal
// mas ainda nao autorizou (sem refresh_token salvo) - mostra o codigo
// de pareamento e espera o usuario aprovar em outro aparelho.
static void runDrivePairingIfNeeded() {
  if (!NetworkRoute::online()) return;
  if (!settingsStore.hasDriveApp() || settingsStore.hasDriveAuth()) return;

  g_cancelDrivePairing = false;
  buttons.setCallback(onDrivePairingButton);
  bool paired = gdrive.pairDevice(settingsStore, drawDrivePairingScreen, serviceDrivePairing);
  buttons.setCallback(onButtonEvent);
  if (paired) {
    Serial.println("[Drive] Novo pareamento concluido e salvo.");
    canvas.clear(EPD_WHITE);
    canvas.drawText(8, 90, "Google Drive", EPD_BLACK, FONT_EMPHASIS);
    canvas.drawText(8, 110, "autorizado!", EPD_BLACK, FONT_BODY);
    epd.displayPart();
    delay(2000);
  } else {
    Serial.printf("[Drive] Pareamento: %s\n", gdrive.lastError().c_str());
  }
}

// Callback que o App chama sob demanda (Sincronizar, menu Wi-Fi) pra
// ligar o radio, escanear e conectar na rede salva mais forte por
// perto. Aproveita a conexao pra tambem acertar o relogio via NTP e
// checar o pareamento do Drive, do jeito que era feito so no boot antes.
static bool onConnectRequested() {
  app.prepareMainUi();
  NetworkRoute::configure(settingsStore.get());
  bool ok = NetworkRoute::connect(wifiMgr, settingsStore);
  if (ok) {
    if (wifiMgr.isConnected()) syncRtcFromNtp();
    runDrivePairingIfNeeded();
  }
  return ok;
}

// Callback do worker: não manipula tela/botões nem inicia OAuth interativo.
static bool onAssistantConnectRequested() {
  return NetworkRoute::connect(wifiMgr, settingsStore, 15000);
}

// Callback que o App chama a partir do menu Wi-Fi ("Portal") pra
// configurar STT/Drive (campos longos demais pro teclado de 2 botoes)
// ou cadastrar Wi-Fi pelo celular. Sobe o Access Point, mostra a tela
// de instrucoes e bloqueia ate BOOT longo (ou ate o portal salvar, que
// reinicia o ESP sozinho).
static void onPortalRequested() {
  app.prepareMainUi();
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

// Trava os 3 rails de energia (EPD/AUDIO/VBAT) no estado atual durante
// o deep sleep. Sem isso nada garante que o pino continua no "off" que
// acabamos de escrever - GPIOs comuns podem flutuar durante o sono e
// isso poderia reenergizar EPD/audio por horas de standby. O
// gpio_hold_dis() correspondente fica no inicio do setup(), antes de
// qualquer power.XOn()/XOff() - sem ele a tela nunca mais ligaria.
//
// Deep sleep already powers flash down in IDF 4.4. This configuration is
// specific to deep sleep, never reused for asynchronous light-sleep wake.
static bool holdPowerRailsAndSleep() {
  if (gpio_hold_en((gpio_num_t)PIN_EPD_PWR) != ESP_OK ||
      gpio_hold_en((gpio_num_t)PIN_AUDIO_PWR) != ESP_OK ||
      gpio_hold_en((gpio_num_t)PIN_VBAT_PWR) != ESP_OK) return false;
  gpio_deep_sleep_hold_en();

  // So os dominios que sabemos que nao precisamos - RTC_SLOW_MEM fica
  // ligado no default (guarda os RTC_DATA_ATTR, ex. rtcWallpaperIndex).
  if (esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF) != ESP_OK ||
      esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF) != ESP_OK ||
      esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF) != ESP_OK) return false;

  // Park SPI only once every fallible sleep configuration has succeeded. A
  // rejected sleep can then restore the panel without broken pin routing.
  for (int pin : {PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_SCK, PIN_EPD_MOSI, PIN_EPD_BUSY})
    pinMode(pin, INPUT);
  ++rtcSleepEntries;
  esp_deep_sleep_start();
}

// Desenha a tela de bloqueio (so o wallpaper, sorteado uma vez por
// periodo de descanso - sem cartao/relogio/status, que agora vivem na
// tela inicial) e desliga a alimentacao do e-paper - o painel e
// biestavel, entao a imagem fica visivel sem energia. Unico jeito de
// acordar: os botoes (ext1) - sem wake por timer, o aparelho so liga
// quando voce mexe nele.
static void enterScreensaver() {
  if (sleepPending) return;
  if (!app.canSleep()) {
    if (app.feedbackBusy() && !app.isAudioActive() && !app.isSyncActive() && !app.isExportActive())
      sleepDeferred = true;
    return;
  }
  sleepDeferred = false;
  app.finishFeedback();
  sleepPending = true;
  sleepGate.reset();
  buttons.suppressUntilRelease(BtnId::Boot);
  buttons.suppressUntilRelease(BtnId::Pwr);
  NetworkRoute::finish();
  wifiMgr.disconnect();
  codec.end();
  Serial.println("[Sleep] Preparando repouso; aguardando soltura dos botoes.");

  if (rtcWallpaperIndex < 0) {
    rtcWallpaperIndex = settingsStore.get().wallpaperChoice >= 0
                             ? settingsStore.get().wallpaperChoice % WALLPAPER_COUNT
                             : random(WALLPAPER_COUNT);
  }

  if (epd.ready() && epd.init()) lockScreen.draw(rtcWallpaperIndex);
  power.epdOff();
  epd.invalidatePower();
  power.audioOff();
}

void setup() {
  BootTrace::mark(BootTrace::Entry);
  // Safe levels must be preloaded BEFORE releasing previous sleep holds.
  power.begin();
  gpio_hold_dis((gpio_num_t)PIN_EPD_PWR);
  gpio_hold_dis((gpio_num_t)PIN_AUDIO_PWR);
  gpio_hold_dis((gpio_num_t)PIN_VBAT_PWR);
  gpio_deep_sleep_hold_dis();
  const auto wake = esp_sleep_get_wakeup_cause();
  const uint64_t wakePins = wake == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
  rtc_gpio_deinit((gpio_num_t)PIN_BTN_BOOT);
  rtc_gpio_deinit((gpio_num_t)PIN_BTN_PWR);
  buttons.begin(PIN_BTN_BOOT, PIN_BTN_PWR, onButtonEvent);
  const bool pwrWake = (wakePins & (1ULL << PIN_BTN_PWR)) ||
                       (wakePins && digitalRead(PIN_BTN_PWR) == LOW);
  const bool captureWake = wakeIntent(wakePins & (1ULL << PIN_BTN_BOOT), pwrWake,
                                      digitalRead(PIN_BTN_PWR) == LOW) == WakeIntent::Capture;
  if (captureWake) buttons.latchWake(BtnId::Boot);
  else if (pwrWake) {
    buttons.suppressUntilRelease(BtnId::Boot);
    buttons.suppressUntilRelease(BtnId::Pwr);
  }
  BootTrace::mark(BootTrace::ButtonObserved);
  buttons.useExternalSampling(true);
  if (xTaskCreatePinnedToCore(sampleButtons, "buttons", 2048, nullptr, 6, &buttonTask, 1) != pdPASS) {
    buttons.useExternalSampling(false); // explicit degraded mode, never fake old ticks
    Serial.println("[Buttons] Sem task de amostragem; usando polling limitado.");
  }
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
#endif
  // Alocacoes de Strings/cJSON/mbedTLS >= 512B usam PSRAM, preservando SRAM DMA para esp-sha/I2S/BLE.
  if (psramFound()) heap_caps_malloc_extmem_enable(512);
  Serial.println("=================================");
  Serial.printf("[Boot] reset_reason=%d wakeup=%d\n", (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());
  Serial.println("Gravador de Ideias - sincronizacao otimizada v3");
  Serial.println("=================================");

  settingsStore.begin();
  BootTrace::mark(BootTrace::SettingsReady);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(50);
  if (!app.beginFeedback(powerOnFeedbackDisplay))
    Serial.println("[DisplayFeedback] Worker indisponivel; captura permanece prioritaria.");
  if (captureWake) app.prepareWakeCapture();
  const bool mounted = LittleFS.begin(false);
  const bool storageReady = mounted && notes.begin();
  app.setStorageReady(storageReady);
  BootTrace::mark(BootTrace::FilesystemReady);
  if (!storageReady) Serial.println("[Storage] Falha ao montar/preparar notas; sem formatacao automatica.");
  if (!rtc.begin(PIN_I2C_SDA, PIN_I2C_SCL))
    Serial.println("!! AVISO: RTC PCF85063 nao respondeu no I2C.");
  NetworkRoute::configure(settingsStore.get());
  // O relógio de sistema também precisa existir quando só houver internet BLE.
  // O PCF85063 usa UTC-3 neste firmware; a conversão não muda TZ global.
  RtcDateTime bootTime;
  if (rtc.getDateTime(bootTime) && bootTime.year >= 2024) {
    struct tm value = {};
    value.tm_year = bootTime.year - 1900; value.tm_mon = bootTime.month - 1;
    value.tm_mday = bootTime.day; value.tm_hour = bootTime.hour;
    value.tm_min = bootTime.minute; value.tm_sec = bootTime.second;
    setenv("TZ", "BRT3", 1);
    tzset();
    const time_t epoch = mktime(&value);
    if (epoch > 0) {
      struct timeval tv = {epoch, 0};
      settimeofday(&tv, nullptr);
    }
  }
  app.begin(enterScreensaver, onConnectRequested, onPortalRequested, onAssistantConnectRequested,
            prepareUi, captureWake);
  // Waking must never divert a local note or PWR->Home into onboarding.
  // First-use portal remains available on cold boot and explicitly in menus.
  if (wake != ESP_SLEEP_WAKEUP_EXT1) { runWifiSetup(); runDrivePairingIfNeeded(); }
}

void loop() {
  if (buttons.eventsLost()) {
    // Losing a release must not leave an unbounded PTT session. Do not
    // dispatch stale commands after overflow; sleep is still explicit/guarded.
    buttonQueueFailed = true;
    app.handleButtonOverflow();
    buttons.discardEvents();
    buttons.suppressUntilRelease(BtnId::Boot);
    buttons.suppressUntilRelease(BtnId::Pwr);
    buttonQueueFailed = false;
    Serial.println("[Buttons] Fila excedida; gesto cancelado explicitamente.");
  }
  buttons.poll();
  if (sleepDeferred && app.canSleep()) enterScreensaver();
  if (sleepPending) {
    const bool released = digitalRead(PIN_BTN_BOOT) == HIGH && digitalRead(PIN_BTN_PWR) == HIGH;
    if (sleepGate.ready(millis(), released, app.canSleep())) {
      bool ok = true;
      for (int pin : {PIN_BTN_BOOT, PIN_BTN_PWR}) {
        ok = (rtc_gpio_pullup_en((gpio_num_t)pin) == ESP_OK) && ok;
        ok = (rtc_gpio_pulldown_dis((gpio_num_t)pin) == ESP_OK) && ok;
      }
      ok = ok && esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_PWR),
                                            ESP_EXT1_WAKEUP_ANY_LOW) == ESP_OK;
      if (ok) ok = holdPowerRailsAndSleep();
      // Only reached on a configuration error, never claim sleep succeeded.
      gpio_deep_sleep_hold_dis();
      gpio_hold_dis((gpio_num_t)PIN_EPD_PWR);
      gpio_hold_dis((gpio_num_t)PIN_AUDIO_PWR);
      gpio_hold_dis((gpio_num_t)PIN_VBAT_PWR);
      rtc_gpio_deinit((gpio_num_t)PIN_BTN_BOOT);
      rtc_gpio_deinit((gpio_num_t)PIN_BTN_PWR);
      sleepPending = false; uiStarted = false;
      // ready() describes the previous controller session, not retained RAM
      // after epdOff(). Rebuild the powered controller on a rejected sleep.
      power.epdOn();
      delay(10);
      if (epd.init()) {
        epd.clear();
        if (!epd.displayPartBaseImage() || !epd.initPartial()) power.epdOff();
      } else power.epdOff();
      prepareUi(); app.redrawHome();
      Serial.printf("[Sleep] Falha na configuracao (%d); aparelho permaneceu acordado.\n", ok);
    }
    delay(5);
    return;
  }
  if (!app.isSyncActive() && !app.isAudioActive() && !app.isExportActive()) wifiMgr.loop();
  serviceNtpRtc();
  app.loop();
#if ARDUINO_USB_CDC_ON_BOOT
  static String command;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      command.trim();
      if (command == "boot-metrics") { printBootMetrics(); Serial.println("[DiagDone] boot-metrics"); }
      else if (command == "feedback-cold") {
        const bool allowed = app.canSleep();
        if (allowed) {
          app.finishFeedback();
          power.epdOff();
          epd.invalidatePower();
          delay(20);
        }
        Serial.printf("[FeedbackTest] cold_started=%d\n", allowed && app.prepareWakeCapture());
        Serial.println("[DiagDone] feedback-cold");
      }
      else if (command == "sleep") {
        const bool accepted = app.canSleep();
        Serial.printf("[DiagDone] sleep requested=%d\n", accepted);
        if (accepted) enterScreensaver();
      }
      else if (command.length()) app.diagnosticCommand(command);
      command = "";
    } else if (c != '\r' && command.length() < 64) {
      command += c;
    }
  }
#endif
}
