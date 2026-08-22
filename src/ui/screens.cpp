#include "screens.h"

namespace Screens {

void drawHome(Canvas &canvas, EPaperDisplay &epd, uint32_t freeSecs, const char *wifiLine,
              int noteCount, int pendingSync) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 6, "Gravador de Ideias", EPD_BLACK, 1);

  char freeLine[32];
  snprintf(freeLine, sizeof(freeLine), "livre: ~%um%02us", freeSecs / 60, freeSecs % 60);
  canvas.drawText(8, 16, freeLine, EPD_BLACK, 1);
  canvas.drawText(8, 26, wifiLine, EPD_BLACK, 1);
  canvas.drawFastHLine(4, 36, 192, EPD_BLACK);

  char line1[32];
  snprintf(line1, sizeof(line1), "%d nota(s) salvas", noteCount);
  canvas.drawText(8, 80, line1, EPD_BLACK, 1);
  if (pendingSync > 0) {
    char line2[32];
    snprintf(line2, sizeof(line2), "%d pendente(s) de sync", pendingSync);
    canvas.drawText(8, 92, line2, EPD_BLACK, 1);
  }

  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "BOOT grava | PWR menu", EPD_BLACK, 1);
  epd.displayPart();
}

void drawRecording(Canvas &canvas, EPaperDisplay &epd, uint32_t elapsedSec, uint32_t freeSec) {
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

  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "BOOT para parar", EPD_BLACK, 1);
  epd.displayPart();
}

void drawText(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *body,
              const char *footerHint) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 6, title, EPD_BLACK, 1);
  canvas.drawFastHLine(4, 18, 192, EPD_BLACK);
  canvas.drawWrappedText(8, 26, body, EPD_BLACK, 1, 30, 10);
  if (footerHint) {
    canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
    canvas.drawText(8, 184, footerHint, EPD_BLACK, 1);
  }
  epd.displayPart();
}

void drawConfirm(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *message) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 6, title, EPD_BLACK, 1);
  canvas.drawFastHLine(4, 18, 192, EPD_BLACK);
  canvas.drawWrappedText(8, 30, message, EPD_BLACK, 1, 30, 11);
  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "BOOT confirma | PWR cancela", EPD_BLACK, 1);
  epd.displayPart();
}

void drawAbout(Canvas &canvas, EPaperDisplay &epd, int noteCount, uint64_t usedBytes,
               uint64_t totalBytes) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 6, "Sobre", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 18, 192, EPD_BLACK);

  canvas.drawText(8, 30, "Gravador de Ideias", EPD_BLACK, 1);

  char l1[32];
  snprintf(l1, sizeof(l1), "%d nota(s) salvas", noteCount);
  canvas.drawText(8, 50, l1, EPD_BLACK, 1);

  char l2[32];
  snprintf(l2, sizeof(l2), "uso: %llu / %llu KB", (unsigned long long)(usedBytes / 1024),
           (unsigned long long)(totalBytes / 1024));
  canvas.drawText(8, 62, l2, EPD_BLACK, 1);

  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "PWR volta", EPD_BLACK, 1);
  epd.displayPart();
}

void drawSyncSummary(Canvas &canvas, EPaperDisplay &epd, int transcribed, int uploaded,
                      int failed, int totalPending) {
  canvas.clear(EPD_WHITE);
  canvas.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas.drawText(8, 6, "Sincronizacao", EPD_BLACK, 1);
  canvas.drawFastHLine(4, 18, 192, EPD_BLACK);

  if (totalPending == 0) {
    canvas.drawText(8, 30, "Nada pendente.", EPD_BLACK, 1);
    canvas.drawText(8, 42, "Tudo ja sincronizado.", EPD_BLACK, 1);
  } else {
    char l1[32];
    snprintf(l1, sizeof(l1), "%d transcrita(s)", transcribed);
    canvas.drawText(8, 30, l1, EPD_BLACK, 1);
    char l2[32];
    snprintf(l2, sizeof(l2), "%d enviada(s)", uploaded);
    canvas.drawText(8, 42, l2, EPD_BLACK, 1);
    if (failed > 0) {
      char l3[32];
      snprintf(l3, sizeof(l3), "%d falharam", failed);
      canvas.drawText(8, 54, l3, EPD_BLACK, 1);
    }
  }

  canvas.drawFastHLine(4, 178, 192, EPD_BLACK);
  canvas.drawText(8, 184, "PWR volta", EPD_BLACK, 1);
  epd.displayPart();
}

} // namespace Screens
