#include "menu.h"

namespace {
constexpr int kRowY0 = 26;
constexpr int kRowH = 13;
constexpr int kFooterHlineY = 178;
constexpr int kFooterTextY = 184;
} // namespace

void Menu::draw(const char *title, const MenuItem *items, int count, int selected,
                 const char *footerHint) {
  canvas_.clear(EPD_WHITE);
  canvas_.drawRect(0, 0, 200, 200, EPD_BLACK);
  canvas_.drawText(8, 6, title, EPD_BLACK, 1);
  canvas_.drawFastHLine(4, 18, 192, EPD_BLACK);

  int visibleRows = (kFooterHlineY - kRowY0) / kRowH;
  int top = 0;
  if (count > visibleRows) {
    top = selected - visibleRows / 2;
    if (top < 0) top = 0;
    if (top > count - visibleRows) top = count - visibleRows;
  }

  int last = min(count, top + visibleRows);
  for (int i = top; i < last; i++) {
    int y = kRowY0 + (i - top) * kRowH;
    bool sel = (i == selected);
    if (sel) canvas_.fillRect(2, y - 1, 196, kRowH, EPD_BLACK);
    uint8_t color = sel ? EPD_WHITE : EPD_BLACK;
    canvas_.drawText(8, y, items[i].label, color, 1);
    if (items[i].value[0] != '\0') {
      int w = Canvas::textWidth(items[i].value, 1);
      canvas_.drawText(192 - w, y, items[i].value, color, 1);
    }
  }

  if (count > visibleRows) {
    // Indicador simples de rolagem: barra vertical proporcional a
    // posicao da janela visivel dentro da lista inteira.
    int trackH = kFooterHlineY - kRowY0;
    int barH = max(6, trackH * visibleRows / count);
    int barY = kRowY0 + (trackH - barH) * top / max(1, count - visibleRows);
    canvas_.drawFastVLine(196, barY, barH, EPD_BLACK);
  }

  canvas_.drawFastHLine(4, kFooterHlineY, 192, EPD_BLACK);
  if (footerHint) canvas_.drawText(8, kFooterTextY, footerHint, EPD_BLACK, 1);

  epd_.displayPart();
}
