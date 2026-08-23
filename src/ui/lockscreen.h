#pragma once
#include <Arduino.h>
#include "canvas.h"
#include "../display/epaper.h"

// Tela de bloqueio: so o wallpaper (ver wallpapers.h) em tela cheia,
// sem cartao de status - hora, bateria e afins agora vivem na tela
// inicial (ver ui/screens.cpp), que e onde o aparelho realmente passa
// tempo aceso. Sem status pra atualizar, nao ha motivo pra acordar so
// pra redesenhar isto - o aparelho so acorda no botao.
class LockScreen {
public:
  LockScreen(EPaperDisplay &epd, Canvas &canvas) : epd_(epd), canvas_(canvas) {}

  // wallpaperIndex < 0 -> tela branca, sem ilustracao. Sempre com a LUT
  // cheia (chamador ja recarregou via epd.init()) - roda so ao entrar
  // no sono, raro o bastante pra valer o refresh mais lento e sem
  // fantasma.
  void draw(int wallpaperIndex);

private:
  EPaperDisplay &epd_;
  Canvas &canvas_;
};
