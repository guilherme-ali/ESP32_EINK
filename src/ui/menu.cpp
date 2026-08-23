#include "menu.h"

namespace {
constexpr int kMarginX = 10;
constexpr int kContentW = 200 - 2 * kMarginX;
constexpr int kHeaderY = 10;
constexpr int kHeaderRuleY = 32;
constexpr int kFooterHlineY = 178;
constexpr int kFooterTextY = 181;
constexpr int kPillRadius = 10;

// Barra de rolagem: uma pilula fina na margem direita, proporcional a
// posicao da janela visivel dentro da lista inteira.
void drawScrollbar(Canvas &canvas, int trackY0, int trackY1, int count, int visible, int top) {
  if (count <= visible) return;
  int trackH = trackY1 - trackY0;
  int barH = max(10, trackH * visible / count);
  int barY = trackY0 + (trackH - barH) * top / max(1, count - visible);
  canvas.fillRoundRect(200 - kMarginX + 2, barY, 3, barH, 1, EPD_BLACK);
}
} // namespace

void Menu::draw(const char *title, const MenuItem *items, int count, int selected,
                 const char *footerHint) {
  canvas_.clear(EPD_WHITE);
  canvas_.drawText(kMarginX, kHeaderY, title, EPD_BLACK, FONT_EMPHASIS);
  canvas_.drawFastHLine(kMarginX, kHeaderRuleY, kContentW, EPD_BLACK);

  constexpr int kRowY0 = 42;
  constexpr int kRowH = 28;
  constexpr int kPillH = kRowH - 3;

  int visibleRows = (kFooterHlineY - kRowY0) / kRowH;
  int top = 0;
  if (count > visibleRows) {
    top = selected - visibleRows / 2;
    if (top < 0) top = 0;
    if (top > count - visibleRows) top = count - visibleRows;
  }

  int pillW = kContentW - 6; // deixa espaco pro indicador de rolagem
  int last = min(count, top + visibleRows);
  for (int i = top; i < last; i++) {
    int y = kRowY0 + (i - top) * kRowH;
    bool sel = (i == selected);
    if (sel) canvas_.fillRoundRect(kMarginX, y, pillW, kPillH, kPillRadius, EPD_BLACK);
    uint8_t color = sel ? EPD_WHITE : EPD_BLACK;
    int textY = y + (kPillH - FONT_BODY.height) / 2;
    canvas_.drawText(kMarginX + 12, textY, items[i].label, color, FONT_BODY);
    if (items[i].value[0] != '\0') {
      int w = Canvas::textWidth(items[i].value, FONT_BODY);
      canvas_.drawText(kMarginX + pillW - 12 - w, textY, items[i].value, color, FONT_BODY);
    }
  }

  drawScrollbar(canvas_, kRowY0, kFooterHlineY, count, visibleRows, top);

  canvas_.drawFastHLine(kMarginX, kFooterHlineY, kContentW, EPD_BLACK);
  if (footerHint) canvas_.drawText(kMarginX, kFooterTextY, footerHint, EPD_BLACK, FONT_BODY);

  epd_.displayPart();
}

void Menu::drawCards(const char *title, const MenuCard *cards, int count, int selected,
                      const char *footerHint) {
  canvas_.clear(EPD_WHITE);
  canvas_.drawText(kMarginX, kHeaderY, title, EPD_BLACK, FONT_EMPHASIS);
  char countBuf[12];
  snprintf(countBuf, sizeof(countBuf), "%d", count);
  int cw = Canvas::textWidth(countBuf, FONT_BODY);
  canvas_.drawText(200 - kMarginX - cw, kHeaderY + 3, countBuf, EPD_BLACK, FONT_BODY);
  canvas_.drawFastHLine(kMarginX, kHeaderRuleY, kContentW, EPD_BLACK);

  constexpr int kRowY0 = 40;
  constexpr int kRowH = 44;
  constexpr int kCardH = kRowH - 4;

  int visibleRows = (kFooterHlineY - kRowY0) / kRowH;
  int top = 0;
  if (count > visibleRows) {
    top = selected - visibleRows / 2;
    if (top < 0) top = 0;
    if (top > count - visibleRows) top = count - visibleRows;
  }

  int cardW = kContentW - 6;
  int last = min(count, top + visibleRows);
  for (int i = top; i < last; i++) {
    int y = kRowY0 + (i - top) * kRowH;
    bool sel = (i == selected);
    if (sel) {
      canvas_.fillRoundRect(kMarginX, y, cardW, kCardH, kPillRadius, EPD_BLACK);
    } else {
      canvas_.drawRoundRect(kMarginX, y, cardW, kCardH, kPillRadius, EPD_BLACK);
    }
    uint8_t color = sel ? EPD_WHITE : EPD_BLACK;
    canvas_.drawText(kMarginX + 12, y + 6, cards[i].line1, color, FONT_BODY);
    canvas_.drawText(kMarginX + 12, y + 22, cards[i].line2, color, FONT_BODY);
  }

  drawScrollbar(canvas_, kRowY0, kFooterHlineY, count, visibleRows, top);

  canvas_.drawFastHLine(kMarginX, kFooterHlineY, kContentW, EPD_BLACK);
  if (footerHint) canvas_.drawText(kMarginX, kFooterTextY, footerHint, EPD_BLACK, FONT_BODY);

  epd_.displayPart();
}
