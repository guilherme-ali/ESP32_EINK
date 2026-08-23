#include "screens.h"

namespace Screens {

namespace {
constexpr int kMarginX = 10;
constexpr int kContentW = 200 - 2 * kMarginX;
constexpr int kHeaderY = 10;
constexpr int kHeaderRuleY = 32;
constexpr int kFooterHlineY = 178;
constexpr int kFooterTextY = 181;

void drawHeader(Canvas &canvas, const char *title) {
  canvas.drawText(kMarginX, kHeaderY, title, EPD_BLACK, FONT_EMPHASIS);
  canvas.drawFastHLine(kMarginX, kHeaderRuleY, kContentW, EPD_BLACK);
}

void drawFooter(Canvas &canvas, const char *hint) {
  canvas.drawFastHLine(kMarginX, kFooterHlineY, kContentW, EPD_BLACK);
  if (hint) canvas.drawText(kMarginX, kFooterTextY, hint, EPD_BLACK, FONT_BODY);
}

void centerText(Canvas &canvas, int y, const char *text, uint8_t color, const Font &font) {
  int w = Canvas::textWidth(text, font);
  canvas.drawText((200 - w) / 2, y, text, color, font);
}

// Sakamoto - dia da semana sem precisar de RTC com registrador proprio.
int dayOfWeek(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7; // 0=domingo
}

const char *kWeekdays[] = {"domingo",   "segunda-feira", "terca-feira", "quarta-feira",
                            "quinta-feira", "sexta-feira", "sabado"};
const char *kMonths[] = {"janeiro",   "fevereiro", "marco",    "abril",   "maio",     "junho",
                          "julho",     "agosto",    "setembro", "outubro", "novembro", "dezembro"};

void formatFullDate(const RtcDateTime &now, char *out, size_t outLen) {
  int dow = dayOfWeek(now.year, now.month, now.day);
  snprintf(out, outLen, "%s, %u de %s", kWeekdays[dow], now.day, kMonths[now.month - 1]);
}
} // namespace

void drawHome(Canvas &canvas, EPaperDisplay &epd, const RtcDateTime &now, bool timeValid,
              int batteryPercent, bool showTempHumidity, bool hasTempHumidity, float tempC,
              float humidity, int noteCount, int pendingSync) {
  canvas.clear(EPD_WHITE);

  char clock[8];
  snprintf(clock, sizeof(clock), timeValid ? "%02u:%02u" : "--:--", now.hour, now.minute);
  centerText(canvas, 16, clock, EPD_BLACK, FONT_CLOCK);

  if (timeValid) {
    char dateLine[40];
    formatFullDate(now, dateLine, sizeof(dateLine));
    centerText(canvas, 64, dateLine, EPD_BLACK, FONT_BODY);
  }

  canvas.drawFastHLine(kMarginX, 84, kContentW, EPD_BLACK);

  int y = 98;
  char line[40];
  snprintf(line, sizeof(line), "%d nota(s) salva(s)", noteCount);
  canvas.drawText(kMarginX + 4, y, line, EPD_BLACK, FONT_BODY);
  y += 18;

  if (pendingSync > 0) {
    snprintf(line, sizeof(line), "%d pendente(s) de sync", pendingSync);
    canvas.drawText(kMarginX + 4, y, line, EPD_BLACK, FONT_BODY);
    y += 18;
  }

  snprintf(line, sizeof(line), "bateria: %d%%", batteryPercent);
  canvas.drawText(kMarginX + 4, y, line, EPD_BLACK, FONT_BODY);
  y += 18;

  if (showTempHumidity && hasTempHumidity) {
    snprintf(line, sizeof(line), "%.1fC  %.0f%% umidade", tempC, humidity);
    canvas.drawText(kMarginX + 4, y, line, EPD_BLACK, FONT_BODY);
  }

  drawFooter(canvas, "BOOT grava | PWR menu");
  epd.displayPart();
}

void drawRecording(Canvas &canvas, EPaperDisplay &epd, uint32_t elapsedSec, uint32_t freeSec) {
  canvas.clear(EPD_WHITE);
  drawHeader(canvas, "gravando...");

  char line[24];
  snprintf(line, sizeof(line), "%um%02us", elapsedSec / 60, elapsedSec % 60);
  centerText(canvas, 80, line, EPD_BLACK, FONT_CLOCK);

  char freeLine[32];
  snprintf(freeLine, sizeof(freeLine), "espaco p/ mais %um%02us", freeSec / 60, freeSec % 60);
  centerText(canvas, 140, freeLine, EPD_BLACK, FONT_BODY);

  drawFooter(canvas, "BOOT para parar");
  epd.displayPart();
}

void drawText(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *body,
              const char *footerHint) {
  canvas.clear(EPD_WHITE);
  drawHeader(canvas, title);
  canvas.drawWrappedText(kMarginX, 42, body, EPD_BLACK, FONT_BODY, kContentW, 18);
  drawFooter(canvas, footerHint);
  epd.displayPart();
}

void drawConfirm(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *message) {
  canvas.clear(EPD_WHITE);
  drawHeader(canvas, title);
  canvas.drawWrappedText(kMarginX, 46, message, EPD_BLACK, FONT_BODY, kContentW, 18);
  drawFooter(canvas, "BOOT confirma | PWR cancela");
  epd.displayPart();
}

void drawAbout(Canvas &canvas, EPaperDisplay &epd, int noteCount, uint64_t usedBytes,
               uint64_t totalBytes) {
  canvas.clear(EPD_WHITE);
  drawHeader(canvas, "sobre");

  canvas.drawText(kMarginX + 4, 44, "Gravador de Ideias", EPD_BLACK, FONT_BODY);

  char l1[32];
  snprintf(l1, sizeof(l1), "%d nota(s) salvas", noteCount);
  canvas.drawText(kMarginX + 4, 68, l1, EPD_BLACK, FONT_BODY);

  char l2[32];
  snprintf(l2, sizeof(l2), "uso: %llu / %llu KB", (unsigned long long)(usedBytes / 1024),
           (unsigned long long)(totalBytes / 1024));
  canvas.drawText(kMarginX + 4, 84, l2, EPD_BLACK, FONT_BODY);

  drawFooter(canvas, "PWR volta");
  epd.displayPart();
}

void drawSyncSummary(Canvas &canvas, EPaperDisplay &epd, int transcribed, int uploaded,
                      int failed, int totalPending) {
  canvas.clear(EPD_WHITE);
  drawHeader(canvas, "sincronizacao");

  int y = 46;
  if (totalPending == 0) {
    canvas.drawText(kMarginX + 4, y, "Nada pendente.", EPD_BLACK, FONT_BODY);
    canvas.drawText(kMarginX + 4, y + 16, "Tudo ja sincronizado.", EPD_BLACK, FONT_BODY);
  } else {
    char l1[32];
    snprintf(l1, sizeof(l1), "%d transcrita(s)", transcribed);
    canvas.drawText(kMarginX + 4, y, l1, EPD_BLACK, FONT_BODY);
    y += 16;
    char l2[32];
    snprintf(l2, sizeof(l2), "%d enviada(s)", uploaded);
    canvas.drawText(kMarginX + 4, y, l2, EPD_BLACK, FONT_BODY);
    y += 16;
    if (failed > 0) {
      char l3[32];
      snprintf(l3, sizeof(l3), "%d falharam", failed);
      canvas.drawText(kMarginX + 4, y, l3, EPD_BLACK, FONT_BODY);
    }
  }

  drawFooter(canvas, "PWR volta");
  epd.displayPart();
}

void drawState(Canvas &canvas, EPaperDisplay &epd, StateIcon icon, const char *label,
               const char *detail, int value, int maxValue) {
  canvas.clear(EPD_WHITE);

  constexpr int kIconCx = 100;
  constexpr int kIconCy = 66;
  if (icon == StateIcon::Activity) {
    canvas.fillCircle(kIconCx - 22, kIconCy, 5, EPD_BLACK);
    canvas.fillCircle(kIconCx, kIconCy, 5, EPD_BLACK);
    canvas.fillCircle(kIconCx + 22, kIconCy, 5, EPD_BLACK);
  } else { // Wifi
    canvas.fillCircle(kIconCx, kIconCy + 14, 4, EPD_BLACK);
    canvas.drawArcTopHalf(kIconCx, kIconCy + 14, 14, EPD_BLACK);
    canvas.drawArcTopHalf(kIconCx, kIconCy + 14, 24, EPD_BLACK);
    canvas.drawArcTopHalf(kIconCx, kIconCy + 14, 34, EPD_BLACK);
  }

  centerText(canvas, 110, label, EPD_BLACK, FONT_EMPHASIS);

  if (detail && detail[0]) {
    canvas.drawWrappedText(kMarginX, 132, detail, EPD_BLACK, FONT_BODY, kContentW, 18);
  }

  if (maxValue > 0) {
    int barW = 160;
    int barY = 158;
    canvas.drawProgressBar((200 - barW) / 2, barY, barW, 14, value, maxValue, EPD_BLACK);
    char counter[16];
    snprintf(counter, sizeof(counter), "%d / %d", value, maxValue);
    centerText(canvas, barY + 22, counter, EPD_BLACK, FONT_BODY);
  }

  epd.displayPart();
}

void drawSaved(Canvas &canvas, EPaperDisplay &epd, int noteNumber) {
  canvas.clear(EPD_WHITE);

  constexpr int kCx = 100;
  constexpr int kCy = 76;
  constexpr int kR = 30;
  canvas.drawCircle(kCx, kCy, kR, EPD_BLACK);
  canvas.drawCircle(kCx, kCy, kR - 1, EPD_BLACK);
  canvas.drawLine(kCx - 14, kCy, kCx - 4, kCy + 12, EPD_BLACK);
  canvas.drawLine(kCx - 4, kCy + 12, kCx + 16, kCy - 12, EPD_BLACK);

  centerText(canvas, 122, "salvo", EPD_BLACK, FONT_EMPHASIS);

  char num[8];
  snprintf(num, sizeof(num), "#%03d", noteNumber);
  centerText(canvas, 146, num, EPD_BLACK, FONT_BODY);

  epd.displayPart();
}

} // namespace Screens
