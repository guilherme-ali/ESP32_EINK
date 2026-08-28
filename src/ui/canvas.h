#pragma once
#include "../display/epaper.h"
#include "fonts.h"

// Desenho de texto e formas sobre o framebuffer do EPaperDisplay. Fonte
// bitmap proporcional (ver fonts.h/tools/font2header.py) em 3 tamanhos -
// FONT_BODY, FONT_EMPHASIS, FONT_CLOCK. Sem dependencia de bibliotecas
// graficas externas.
class Canvas {
public:
  explicit Canvas(EPaperDisplay &epd) : epd_(epd) {}

  void clear(uint8_t color = EPD_WHITE);
  void drawPixel(int x, int y, uint8_t color);
  void drawFastHLine(int x, int y, int w, uint8_t color);
  void drawFastVLine(int x, int y, int h, uint8_t color);
  void drawRect(int x, int y, int w, int h, uint8_t color);
  void fillRect(int x, int y, int w, int h, uint8_t color);

  // r e limitado a min(w,h)/2 internamente - sem checagem de faixa, quem
  // chama e responsavel por nao pedir um raio maior que a forma.
  void drawRoundRect(int x, int y, int w, int h, int r, uint8_t color);
  void fillRoundRect(int x, int y, int w, int h, int r, uint8_t color);

  void drawCircle(int cx, int cy, int r, uint8_t color);
  void fillCircle(int cx, int cy, int r, uint8_t color);
  // So os dois quadrantes de cima (icone de wifi: arcos concentricos
  // sobre um ponto). Sem arco em angulo arbitrario - so essa metade.
  void drawArcTopHalf(int cx, int cy, int r, uint8_t color);

  void drawLine(int x0, int y0, int x1, int y1, uint8_t color);

  // Contorno arredondado + preenchimento proporcional a value/maxValue
  // (0 quando maxValue <= 0), sem o contador "N / M" - isso e texto,
  // fica por conta de quem chama.
  void drawProgressBar(int x, int y, int w, int h, int value, int maxValue, uint8_t color);

  void drawChar(int x, int y, uint16_t codepoint, uint8_t color, const Font &font);
  void drawText(int x, int y, const char *text, uint8_t color, const Font &font);

  // Quebra `text` em varias linhas de ate `maxWidthPx` pixels (quebra em
  // espaco ou \n) e desenha a partir de (x,y). Permite paginacao via
  // startLine e maxLines. Retorna o numero de linhas desenhadas.
  int drawWrappedText(int x, int y, const char *text, uint8_t color,
                       const Font &font, int maxWidthPx, int lineHeight,
                       int startLine = 0, int maxLines = 0, int *totalLinesOut = nullptr);

  static int textWidth(const char *text, const Font &font);
  static int charWidth(uint16_t codepoint, const Font &font);
  static uint16_t nextUtf8(const char *&p);

private:
  EPaperDisplay &epd_;
};
