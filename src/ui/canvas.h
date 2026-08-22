#pragma once
#include "../display/epaper.h"

// Desenho simples de texto e formas sobre o framebuffer do EPaperDisplay.
// Fonte fixa 5x7 (ver font5x7.h). Sem dependencia de bibliotecas graficas
// externas - o suficiente para telas de status, listas e texto transcrito.
class Canvas {
public:
  explicit Canvas(EPaperDisplay &epd) : epd_(epd) {}

  void clear(uint8_t color = EPD_WHITE);
  void drawPixel(int x, int y, uint8_t color);
  void drawFastHLine(int x, int y, int w, uint8_t color);
  void drawFastVLine(int x, int y, int h, uint8_t color);
  void drawRect(int x, int y, int w, int h, uint8_t color);
  void fillRect(int x, int y, int w, int h, uint8_t color);

  // scale=1 -> glifo 5x7; scale=2 -> 10x14, etc.
  void drawChar(int x, int y, char c, uint8_t color, int scale = 1);
  void drawText(int x, int y, const char *text, uint8_t color, int scale = 1);

  // Quebra `text` em varias linhas de ate `maxCharsPerLine` caracteres
  // (quebra em espaco quando possivel) e desenha a partir de (x,y).
  int drawWrappedText(int x, int y, const char *text, uint8_t color,
                       int scale, int maxCharsPerLine, int lineHeight);

  static int textWidth(const char *text, int scale = 1);
  static constexpr int charWidth(int scale = 1) { return 6 * scale; } // 5px + 1 espaco
  static constexpr int charHeight(int scale = 1) { return 8 * scale; } // 7px + 1 espaco

private:
  EPaperDisplay &epd_;
};
