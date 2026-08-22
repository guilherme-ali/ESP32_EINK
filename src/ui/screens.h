#pragma once
#include "canvas.h"
#include "../display/epaper.h"

// Telas "soltas" (sem lista) da UI interativa - as telas de lista usam
// o widget Menu (ver ui/menu.h) diretamente de dentro do App. Cada
// funcao desenha e ja aciona epd.displayPart() (refresh parcial, igual
// ao resto do app desde o boot).
namespace Screens {

void drawHome(Canvas &canvas, EPaperDisplay &epd, uint32_t freeSecs, const char *wifiLine,
              int noteCount, int pendingSync);

void drawRecording(Canvas &canvas, EPaperDisplay &epd, uint32_t elapsedSec, uint32_t freeSec);

// title no topo; body com quebra de linha automatica. footerHint opcional
// (linha inferior, ex: "PWR volta").
void drawText(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *body,
              const char *footerHint = nullptr);

// Tela de confirmacao de uma acao (apagar nota, apagar tudo, etc.).
void drawConfirm(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *message);

void drawAbout(Canvas &canvas, EPaperDisplay &epd, int noteCount, uint64_t usedBytes,
               uint64_t totalBytes);

void drawSyncSummary(Canvas &canvas, EPaperDisplay &epd, int transcribed, int uploaded,
                      int failed, int totalPending);

} // namespace Screens
