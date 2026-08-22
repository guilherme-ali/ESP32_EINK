#pragma once
#include "canvas.h"
#include "../display/epaper.h"

// Widget de lista para menus e telas de configuracao: titulo, itens com
// '>' marcando o selecionado e um valor opcional alinhado a direita
// (usado nas telas de configuracao). Rola a janela visivel para manter
// o item selecionado sempre visivel. So desenha - selecao/navegacao
// ficam com quem chama (App).
struct MenuItem {
  const char *label;
  char value[24]; // "" = item de navegacao, sem valor a direita
};

class Menu {
public:
  Menu(Canvas &canvas, EPaperDisplay &epd) : canvas_(canvas), epd_(epd) {}

  // Sempre usa refresh parcial (rapido) - e o modo usado em toda a UI
  // interativa desde o boot (ver EPaperDisplay::initPartial() em main.cpp).
  void draw(const char *title, const MenuItem *items, int count, int selected,
            const char *footerHint);

private:
  Canvas &canvas_;
  EPaperDisplay &epd_;
};
