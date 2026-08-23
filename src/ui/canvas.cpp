#include "canvas.h"
#include <string.h>

void Canvas::clear(uint8_t color) {
  memset(epd_.buffer(), color == EPD_WHITE ? 0xFF : 0x00, epd_.bufferLen());
}

void Canvas::drawPixel(int x, int y, uint8_t color) {
  if (x < 0 || y < 0) return;
  epd_.drawPixel((uint16_t)x, (uint16_t)y, color);
}

// Preenche por byte em vez de pixel a pixel: so as bordas (byte inicial
// e final, quando nao alinhados a 8) passam por drawPixel(); o miolo
// vira um memset() de 1 byte por 8 pixels. fillRect() (varias linhas)
// e a barra de selecao do menu - a chamada mais quente da UI - herdam
// o ganho automaticamente, ja que so chamam isto por linha.
void Canvas::drawFastHLine(int x, int y, int w, uint8_t color) {
  if (y < 0 || y >= epd_.height() || w <= 0) return;

  int x0 = x;
  int x1 = x + w - 1;
  if (x0 < 0) x0 = 0;
  if (x1 >= epd_.width()) x1 = epd_.width() - 1;
  if (x0 > x1) return;

  int byteStart = x0 >> 3;
  int byteEnd = x1 >> 3;

  if (byteStart == byteEnd) {
    for (int px = x0; px <= x1; px++) drawPixel(px, y, color);
    return;
  }

  int firstByteBoundary = (byteStart + 1) * 8 - 1;
  for (int px = x0; px <= firstByteBoundary; px++) drawPixel(px, y, color);

  int midStart = byteStart + 1;
  if (byteEnd > midStart) {
    int rowBytes = epd_.width() / 8;
    uint8_t *row = epd_.buffer() + (size_t)y * rowBytes;
    uint8_t fill = (color == EPD_WHITE) ? 0xFF : 0x00;
    memset(row + midStart, fill, byteEnd - midStart);
  }

  int lastByteBoundaryStart = byteEnd * 8;
  for (int px = lastByteBoundaryStart; px <= x1; px++) drawPixel(px, y, color);
}

void Canvas::drawFastVLine(int x, int y, int h, uint8_t color) {
  for (int i = 0; i < h; i++) drawPixel(x, y + i, color);
}

void Canvas::drawRect(int x, int y, int w, int h, uint8_t color) {
  drawFastHLine(x, y, w, color);
  drawFastHLine(x, y + h - 1, w, color);
  drawFastVLine(x, y, h, color);
  drawFastVLine(x + w - 1, y, h, color);
}

void Canvas::fillRect(int x, int y, int w, int h, uint8_t color) {
  for (int j = 0; j < h; j++) drawFastHLine(x, y + j, w, color);
}

// Algoritmo de circulo por quadrante (midpoint circle), portado do
// Adafruit-GFX-Library (BSD) - mesma origem da fonte antiga (font5x7.h).
// cornername usa a mascara classica da lib: 1=sup-esq 2=sup-dir 4=inf-dir
// 8=inf-esq (bits combinaveis).
static void circleHelper(Canvas &c, int x0, int y0, int r, int cornername, uint8_t color) {
  int f = 1 - r;
  int ddF_x = 1;
  int ddF_y = -2 * r;
  int x = 0;
  int y = r;

  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++;
    ddF_x += 2;
    f += ddF_x;
    if (cornername & 0x4) {
      c.drawPixel(x0 + x, y0 + y, color);
      c.drawPixel(x0 + y, y0 + x, color);
    }
    if (cornername & 0x2) {
      c.drawPixel(x0 + x, y0 - y, color);
      c.drawPixel(x0 + y, y0 - x, color);
    }
    if (cornername & 0x8) {
      c.drawPixel(x0 - y, y0 + x, color);
      c.drawPixel(x0 - x, y0 + y, color);
    }
    if (cornername & 0x1) {
      c.drawPixel(x0 - y, y0 - x, color);
      c.drawPixel(x0 - x, y0 - y, color);
    }
  }
}

static void fillCircleHelper(Canvas &c, int x0, int y0, int r, int cornername, int delta, uint8_t color) {
  int f = 1 - r;
  int ddF_x = 1;
  int ddF_y = -2 * r;
  int x = 0;
  int y = r;
  int px = x;
  int py = y;

  delta++; // evita sobreposicao quando usado por fillRoundRect

  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++;
    ddF_x += 2;
    f += ddF_x;
    if (x < (y + 1)) {
      if (cornername & 0x1) c.drawFastVLine(x0 + x, y0 - y, 2 * y + delta, color);
      if (cornername & 0x2) c.drawFastVLine(x0 - x, y0 - y, 2 * y + delta, color);
    }
    if (y != py) {
      if (cornername & 0x1) c.drawFastVLine(x0 + py, y0 - px, 2 * px + delta, color);
      if (cornername & 0x2) c.drawFastVLine(x0 - py, y0 - px, 2 * px + delta, color);
      py = y;
    }
    px = x;
  }
}

void Canvas::drawCircle(int cx, int cy, int r, uint8_t color) {
  int f = 1 - r;
  int ddF_x = 1;
  int ddF_y = -2 * r;
  int x = 0;
  int y = r;

  drawPixel(cx, cy + r, color);
  drawPixel(cx, cy - r, color);
  drawPixel(cx + r, cy, color);
  drawPixel(cx - r, cy, color);

  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++;
    ddF_x += 2;
    f += ddF_x;
    drawPixel(cx + x, cy + y, color);
    drawPixel(cx - x, cy + y, color);
    drawPixel(cx + x, cy - y, color);
    drawPixel(cx - x, cy - y, color);
    drawPixel(cx + y, cy + x, color);
    drawPixel(cx - y, cy + x, color);
    drawPixel(cx + y, cy - x, color);
    drawPixel(cx - y, cy - x, color);
  }
}

void Canvas::fillCircle(int cx, int cy, int r, uint8_t color) {
  drawFastVLine(cx, cy - r, 2 * r + 1, color);
  fillCircleHelper(*this, cx, cy, r, 3, 0, color);
}

void Canvas::drawArcTopHalf(int cx, int cy, int r, uint8_t color) {
  drawPixel(cx + r, cy, color);
  drawPixel(cx - r, cy, color);
  drawPixel(cx, cy - r, color);
  circleHelper(*this, cx, cy, r, 0x1 | 0x2, color);
}

void Canvas::drawLine(int x0, int y0, int x1, int y1, uint8_t color) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void Canvas::drawRoundRect(int x, int y, int w, int h, int r, uint8_t color) {
  int maxR = (w < h ? w : h) / 2;
  if (r > maxR) r = maxR;
  if (r < 0) r = 0;

  drawFastHLine(x + r, y, w - 2 * r, color);
  drawFastHLine(x + r, y + h - 1, w - 2 * r, color);
  drawFastVLine(x, y + r, h - 2 * r, color);
  drawFastVLine(x + w - 1, y + r, h - 2 * r, color);

  circleHelper(*this, x + r, y + r, r, 1, color);
  circleHelper(*this, x + w - r - 1, y + r, r, 2, color);
  circleHelper(*this, x + w - r - 1, y + h - r - 1, r, 4, color);
  circleHelper(*this, x + r, y + h - r - 1, r, 8, color);
}

void Canvas::fillRoundRect(int x, int y, int w, int h, int r, uint8_t color) {
  int maxR = (w < h ? w : h) / 2;
  if (r > maxR) r = maxR;
  if (r < 0) r = 0;

  fillRect(x + r, y, w - 2 * r, h, color);
  fillCircleHelper(*this, x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
  fillCircleHelper(*this, x + r, y + r, r, 2, h - 2 * r - 1, color);
}

void Canvas::drawProgressBar(int x, int y, int w, int h, int value, int maxValue, uint8_t color) {
  int r = h / 2;
  drawRoundRect(x, y, w, h, r, color);
  if (maxValue <= 0) return;

  int v = value;
  if (v < 0) v = 0;
  if (v > maxValue) v = maxValue;
  if (v == 0) return;

  const int pad = 2;
  int innerW = w - 2 * pad;
  int fillW = (int)((long)innerW * v / maxValue);
  if (fillW <= 0) return;
  int innerH = h - 2 * pad;
  int innerR = innerH / 2;
  fillRoundRect(x + pad, y + pad, fillW, innerH, innerR, color);
}

void Canvas::drawChar(int x, int y, char c, uint8_t color, const Font &font) {
  if ((uint8_t)c < FONT_FIRST_CHAR || (uint8_t)c > FONT_LAST_CHAR) c = '?';
  const FontGlyph &glyph = font.glyphs[(uint8_t)c - FONT_FIRST_CHAR];

  for (int col = 0; col < glyph.width; col++) {
    const uint8_t *colBytes = glyph.bitmap + (size_t)col * font.bytesPerCol;
    for (int row = 0; row < font.height; row++) {
      if (colBytes[row >> 3] & (1 << (row & 7))) {
        drawPixel(x + col, y + row, color);
      }
    }
  }
}

void Canvas::drawText(int x, int y, const char *text, uint8_t color, const Font &font) {
  int cx = x;
  for (const char *p = text; *p; p++) {
    if (*p == '\n') {
      cx = x;
      y += font.height;
      continue;
    }
    drawChar(cx, y, *p, color, font);
    cx += charWidth(*p, font);
  }
}

int Canvas::charWidth(char c, const Font &font) {
  if ((uint8_t)c < FONT_FIRST_CHAR || (uint8_t)c > FONT_LAST_CHAR) c = '?';
  return font.glyphs[(uint8_t)c - FONT_FIRST_CHAR].width;
}

int Canvas::textWidth(const char *text, const Font &font) {
  int w = 0;
  for (const char *p = text; *p; p++) w += charWidth(*p, font);
  return w;
}

int Canvas::drawWrappedText(int x, int y, const char *text, uint8_t color,
                             const Font &font, int maxWidthPx, int lineHeight) {
  int line = 0;
  char lineBuf[160];
  int lineLen = 0;
  int lineWidth = 0;

  auto flushLine = [&]() {
    lineBuf[lineLen] = '\0';
    drawText(x, y + line * lineHeight, lineBuf, color, font);
    line++;
    lineLen = 0;
    lineWidth = 0;
  };

  size_t len = strlen(text);
  size_t i = 0;
  int spaceWidth = charWidth(' ', font);

  while (i < len) {
    size_t wordStart = i;
    while (i < len && text[i] != ' ') i++;
    size_t wordLen = i - wordStart;

    int wordWidth = 0;
    for (size_t k = 0; k < wordLen; k++) wordWidth += charWidth(text[wordStart + k], font);

    if (lineLen > 0 && lineWidth + spaceWidth + wordWidth > maxWidthPx) flushLine();

    if (wordWidth > maxWidthPx && lineLen == 0) {
      // palavra maior que a linha inteira: forca quebra por caractere
      for (size_t k = 0; k < wordLen; k++) {
        char ch = text[wordStart + k];
        int cw = charWidth(ch, font);
        if (lineLen > 0 && lineWidth + cw > maxWidthPx) flushLine();
        if (lineLen < (int)sizeof(lineBuf) - 1) {
          lineBuf[lineLen++] = ch;
          lineWidth += cw;
        }
      }
    } else {
      if (lineLen > 0 && lineLen < (int)sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = ' ';
        lineWidth += spaceWidth;
      }
      for (size_t k = 0; k < wordLen && lineLen < (int)sizeof(lineBuf) - 1; k++) {
        lineBuf[lineLen++] = text[wordStart + k];
      }
      lineWidth += wordWidth;
    }

    while (i < len && text[i] == ' ') i++;
  }
  if (lineLen > 0) flushLine();

  return line;
}
