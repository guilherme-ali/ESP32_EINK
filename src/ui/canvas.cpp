#include "canvas.h"
#include "font5x7.h"
#include <string.h>

void Canvas::clear(uint8_t color) {
  memset(epd_.buffer(), color == EPD_WHITE ? 0xFF : 0x00, epd_.bufferLen());
}

void Canvas::drawPixel(int x, int y, uint8_t color) {
  if (x < 0 || y < 0) return;
  epd_.drawPixel((uint16_t)x, (uint16_t)y, color);
}

void Canvas::drawFastHLine(int x, int y, int w, uint8_t color) {
  for (int i = 0; i < w; i++) drawPixel(x + i, y, color);
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

void Canvas::drawChar(int x, int y, char c, uint8_t color, int scale) {
  // font[] (ver font5x7.h) cobre os 256 codigos ASCII diretamente -
  // indice = codigo do caractere, sem subtrair 0x20.
  if (c < 0x20 || c > 0x7E) c = '?';
  const uint8_t *glyph = &font[(uint8_t)c * 5];

  for (int col = 0; col < 5; col++) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 8; row++) {
      if (bits & (1 << row)) {
        if (scale == 1) {
          drawPixel(x + col, y + row, color);
        } else {
          fillRect(x + col * scale, y + row * scale, scale, scale, color);
        }
      }
    }
  }
}

void Canvas::drawText(int x, int y, const char *text, uint8_t color, int scale) {
  int cx = x;
  for (const char *p = text; *p; p++) {
    if (*p == '\n') {
      cx = x;
      y += charHeight(scale);
      continue;
    }
    drawChar(cx, y, *p, color, scale);
    cx += charWidth(scale);
  }
}

int Canvas::textWidth(const char *text, int scale) {
  int len = strlen(text);
  return len > 0 ? len * charWidth(scale) - scale : 0;
}

int Canvas::drawWrappedText(int x, int y, const char *text, uint8_t color,
                             int scale, int maxCharsPerLine, int lineHeight) {
  int len = strlen(text);
  int pos = 0;
  int line = 0;

  while (pos < len) {
    int remaining = len - pos;
    int take = remaining < maxCharsPerLine ? remaining : maxCharsPerLine;

    if (take == maxCharsPerLine && pos + take < len) {
      int cut = take;
      while (cut > 0 && text[pos + cut] != ' ') cut--;
      if (cut > 0) take = cut;
    }

    char lineBuf[128];
    int copyLen = take < (int)sizeof(lineBuf) - 1 ? take : (int)sizeof(lineBuf) - 1;
    memcpy(lineBuf, text + pos, copyLen);
    lineBuf[copyLen] = '\0';

    drawText(x, y + line * lineHeight, lineBuf, color, scale);

    pos += take;
    while (pos < len && text[pos] == ' ') pos++;
    line++;
  }

  return line;
}
