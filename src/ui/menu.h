#pragma once
#include "canvas.h"
#include "../display/epaper.h"

// Widget de lista para menus e telas de configuracao: cabecalho leve +
// itens em pilulas arredondadas, com o selecionado preenchido de preto
// (texto branco). Rola a janela visivel para manter o item selecionado
// sempre visivel. So desenha - selecao/navegacao ficam com quem chama
// (App).
struct MenuItem {
  const char *label;
  char value[24]; // "" = item de navegacao, sem valor a direita
};

// Cartao de 2 linhas (lista de notas): "#006  1m32s" em cima, data/hora
// por extenso embaixo - ver Menu::drawCards().
struct MenuCard {
  char line1[32];
  char line2[24];
};

class Menu {
public:
  Menu(Canvas &canvas, EPaperDisplay &epd) : canvas_(canvas), epd_(epd) {}

  // Sempre usa refresh parcial (rapido) - e o modo usado em toda a UI
  // interativa desde o boot (ver EPaperDisplay::initPartial() em main.cpp).
  void draw(const char *title, const MenuItem *items, int count, int selected,
            const char *footerHint);

  void drawCards(const char *title, const MenuCard *cards, int count, int selected,
                 const char *footerHint);

private:
  Canvas &canvas_;
  EPaperDisplay &epd_;
};
