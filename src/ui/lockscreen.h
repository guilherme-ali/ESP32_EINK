#pragma once
#include <Arduino.h>
#include "canvas.h"
#include "../display/epaper.h"
#include "../board/rtc.h"

// Composicao da tela de bloqueio: um plano de fundo (ver wallpapers.h)
// com um cartao branco no meio mostrando hora, data e status. O cartao
// garante legibilidade em cima de qualquer um dos planos de fundo, que
// tem arte espalhada pela tela toda.
struct LockScreenStatus {
  RtcDateTime time;
  bool hasTime;
  int batteryPercent; // -1 = sem leitura
  float tempC;
  float humidity;
  bool hasTempHumidity;
  int pendingSyncCount; // notas sem .snc
};

class LockScreen {
public:
  LockScreen(EPaperDisplay &epd, Canvas &canvas) : epd_(epd), canvas_(canvas) {}

  // wallpaperIndex < 0 -> so o cartao, sem ilustracao de fundo.
  // fullRefresh=true usa a LUT cheia (mais lento, sem fantasma) - usado
  // ao entrar no sono. fullRefresh=false usa a LUT parcial (mais
  // rapido) para os wakes periodicos do relogio, que o chamador ja
  // preparou com initPartial() antes de chamar isto.
  void draw(int wallpaperIndex, const LockScreenStatus &status, bool fullRefresh = true);

private:
  EPaperDisplay &epd_;
  Canvas &canvas_;
};
