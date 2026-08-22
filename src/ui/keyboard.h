#pragma once
#include "canvas.h"
#include "../display/epaper.h"

// Entrada de texto usando so os dois botoes do aparelho: uma grade de
// caracteres onde PWR curto anda pra celula seguinte, PWR longo pula
// pra proxima linha (navegacao rapida) e BOOT curto aciona a celula
// atual (insere o caractere, ou a acao especial: mai/minusc, espaco,
// apagar, OK, cancelar). Usado para cadastrar redes Wi-Fi sem precisar
// do portal web.
class Keyboard {
public:
  Keyboard(Canvas &canvas, EPaperDisplay &epd) : canvas_(canvas), epd_(epd) {}

  // buffer deve sobreviver enquanto o teclado estiver ativo - o texto e
  // escrito nele conforme o usuario digita. mask=true mostra so a
  // quantidade de caracteres (usado pra senha).
  void begin(const char *title, char *buffer, size_t bufferLen, bool mask);

  void onNext();    // PWR curto
  void onNextRow(); // PWR longo
  void onSelect();  // BOOT curto

  bool isDone() const { return done_; }
  bool wasConfirmed() const { return confirmed_; }

private:
  Canvas &canvas_;
  EPaperDisplay &epd_;

  const char *title_ = "";
  char *buffer_ = nullptr;
  size_t bufferLen_ = 0;
  size_t len_ = 0;
  bool mask_ = false;

  int cursor_ = 0;
  bool caseUpper_ = false;
  bool done_ = false;
  bool confirmed_ = false;

  void draw();
};
