#include "lockscreen.h"
#include "wallpapers.h"
#include <string.h>

namespace {
// Cartao branco no meio da tela onde ficam hora/data/status - garante
// leitura em cima de qualquer um dos planos de fundo (todos tem arte
// espalhada pela tela toda, entao um recorte fixo e mais simples e
// previsivel do que tentar desviar da ilustracao caso a caso).
constexpr int kCardY = 70;
constexpr int kCardH = 62;
} // namespace

void LockScreen::draw(int wallpaperIndex, const LockScreenStatus &status, bool fullRefresh) {
  if (wallpaperIndex >= 0 && wallpaperIndex < WALLPAPER_COUNT) {
    memcpy_P(epd_.buffer(), WALLPAPERS[wallpaperIndex], epd_.bufferLen());
  } else {
    canvas_.clear(EPD_WHITE);
  }

  canvas_.fillRect(0, kCardY, 200, kCardH, EPD_WHITE);
  canvas_.drawFastHLine(0, kCardY, 200, EPD_BLACK);
  canvas_.drawFastHLine(0, kCardY + kCardH - 1, 200, EPD_BLACK);

  char clock[8] = "--:--";
  char date[16] = "";
  if (status.hasTime) {
    snprintf(clock, sizeof(clock), "%02u:%02u", status.time.hour, status.time.minute);
    snprintf(date, sizeof(date), "%02u/%02u/%04u", status.time.day, status.time.month,
              status.time.year);
  }
  int clockW = Canvas::textWidth(clock, 3);
  canvas_.drawText((200 - clockW) / 2, kCardY + 6, clock, EPD_BLACK, 3);

  int dateW = Canvas::textWidth(date, 1);
  canvas_.drawText((200 - dateW) / 2, kCardY + 34, date, EPD_BLACK, 1);

  char line[40];
  int pos = 0;
  if (status.batteryPercent >= 0) {
    pos += snprintf(line + pos, sizeof(line) - pos, "bat %d%%", status.batteryPercent);
  }
  if (status.hasTempHumidity) {
    pos += snprintf(line + pos, sizeof(line) - pos, "%s%.0fC %.0f%%", pos > 0 ? "  " : "",
                     status.tempC, status.humidity);
  }
  if (status.pendingSyncCount > 0) {
    snprintf(line + pos, sizeof(line) - pos, "%s%d p/ sync", pos > 0 ? "  " : "",
              status.pendingSyncCount);
  }
  int lineW = Canvas::textWidth(line, 1);
  canvas_.drawText((200 - lineW) / 2, kCardY + 46, line, EPD_BLACK, 1);

  if (fullRefresh) {
    // LUT cheia - usado ao entrar no sono (raro, vale o "flash" extra
    // pra nao acumular fantasma).
    epd_.display();
  } else {
    // LUT parcial - os wakes periodicos do relogio (a cada poucos
    // minutos, por horas) sao bem mais frequentes que a entrada no
    // sono; o chamador forca uma passada com fullRefresh=true de vez
    // em quando pra limpar fantasma acumulado (ver kLockFullRefreshEvery
    // em main.cpp).
    epd_.displayPart();
  }
}
