#pragma once
#include "canvas.h"
#include "../display/epaper.h"
#include "../board/rtc.h"

// Telas "soltas" (sem lista) da UI interativa - as telas de lista usam
// o widget Menu (ver ui/menu.h) diretamente de dentro do App. Cada
// funcao desenha e ja aciona epd.displayPart() (refresh parcial, igual
// ao resto do app desde o boot).
namespace Screens {

// Hora grande + data por extenso + status que antes vivia na tela de
// bloqueio (notas, pendentes, bateria, temperatura/umidade opcional).
// timeValid=false (RTC nunca sincronizado) mostra "--:--" e omite a data.
void drawHome(Canvas &canvas, EPaperDisplay &epd, const RtcDateTime &now, bool timeValid,
              int batteryPercent, bool showTempHumidity, bool hasTempHumidity, float tempC,
              float humidity, int noteCount, int pendingSync);

void drawRecording(Canvas &canvas, EPaperDisplay &epd, uint32_t elapsedSec, uint32_t freeSec);

// title no topo; body com quebra de linha automatica. footerHint opcional
// (linha inferior, ex: "PWR volta").
void drawText(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *body,
              const char *footerHint = nullptr);

// Visualizador de texto paginado com suporte a rolagem de paginas.
void drawPagedText(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *body,
                   int pageIndex, int &totalPagesOut, const char *footerHint = nullptr);

// Tela de confirmacao de uma acao (apagar nota, apagar tudo, etc.).
void drawConfirm(Canvas &canvas, EPaperDisplay &epd, const char *title, const char *message);

void drawAbout(Canvas &canvas, EPaperDisplay &epd, int noteCount, uint64_t usedBytes,
               uint64_t totalBytes);

void drawSyncSummary(Canvas &canvas, EPaperDisplay &epd, int transcribed, int uploaded,
                      int failed, int totalPending);

enum class StateIcon { Activity, Wifi };

// Tela de estado centrada: icone de traco simples + rotulo + detalhe
// opcional + barra de progresso opcional (maxValue <= 0 = sem barra,
// so o icone "vivo").
void drawState(Canvas &canvas, EPaperDisplay &epd, StateIcon icon, const char *label,
               const char *detail = nullptr, int value = 0, int maxValue = 0);

// Confirmacao curta pos-gravacao: check em circulo + "salvo" + "#NNN".
void drawSaved(Canvas &canvas, EPaperDisplay &epd, int noteNumber);

} // namespace Screens
