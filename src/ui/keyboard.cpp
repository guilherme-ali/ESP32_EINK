#include "keyboard.h"
#include <string.h>

namespace {
constexpr const char *kLower = "abcdefghijklmnopqrstuvwxyz0123456789.-_@";
constexpr const char *kUpper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_@";
constexpr int kCharCount = 40; // 26 letras + 10 digitos + 4 simbolos

constexpr int kCase = kCharCount;
constexpr int kSpace = kCharCount + 1;
constexpr int kDel = kCharCount + 2;
constexpr int kOk = kCharCount + 3;
constexpr int kCancel = kCharCount + 4;
constexpr int kTotalCells = kCharCount + 5; // 45

constexpr int kCols = 8;
constexpr int kCellW = 200 / kCols; // 25
constexpr int kGridY0 = 34;
constexpr int kCellH = 24;
constexpr int kRows = 6; // ceil(45/8)
} // namespace

void Keyboard::begin(const char *title, char *buffer, size_t bufferLen, bool mask) {
  title_ = title;
  buffer_ = buffer;
  bufferLen_ = bufferLen;
  mask_ = mask;
  len_ = strlen(buffer_);
  cursor_ = 0;
  caseUpper_ = false;
  done_ = false;
  confirmed_ = false;
  draw();
}

void Keyboard::onNext() {
  cursor_ = (cursor_ + 1) % kTotalCells;
  draw();
}

void Keyboard::onNextRow() {
  int row = cursor_ / kCols;
  row = (row + 1) % kRows;
  cursor_ = row * kCols;
  if (cursor_ >= kTotalCells) cursor_ = kTotalCells - 1;
  draw();
}

void Keyboard::onSelect() {
  if (cursor_ < kCharCount) {
    if (len_ < bufferLen_ - 1) {
      buffer_[len_++] = (caseUpper_ ? kUpper : kLower)[cursor_];
      buffer_[len_] = '\0';
    }
  } else if (cursor_ == kCase) {
    caseUpper_ = !caseUpper_;
  } else if (cursor_ == kSpace) {
    if (len_ < bufferLen_ - 1) {
      buffer_[len_++] = ' ';
      buffer_[len_] = '\0';
    }
  } else if (cursor_ == kDel) {
    if (len_ > 0) {
      len_--;
      buffer_[len_] = '\0';
    }
  } else if (cursor_ == kOk) {
    confirmed_ = true;
    done_ = true;
  } else if (cursor_ == kCancel) {
    confirmed_ = false;
    done_ = true;
  }
  draw();
}

void Keyboard::draw() {
  canvas_.clear(EPD_WHITE);
  canvas_.drawText(10, 8, title_, EPD_BLACK, FONT_EMPHASIS);

  char shown[40];
  if (mask_) {
    snprintf(shown, sizeof(shown), "(%u caractere%s)", (unsigned)len_, len_ == 1 ? "" : "s");
  } else {
    size_t start = len_ > 30 ? len_ - 30 : 0;
    snprintf(shown, sizeof(shown), "%s", buffer_ + start);
  }
  canvas_.drawText(10, 26, shown, EPD_BLACK, FONT_BODY);
  canvas_.drawFastHLine(10, 32, 180, EPD_BLACK);

  constexpr int kCellPad = 2;
  constexpr int kCellRadius = 4;
  for (int i = 0; i < kTotalCells; i++) {
    int col = i % kCols;
    int row = i / kCols;
    int x = col * kCellW;
    int y = kGridY0 + row * kCellH;
    bool sel = (i == cursor_);
    if (sel) {
      canvas_.fillRoundRect(x + kCellPad, y + kCellPad, kCellW - 2 * kCellPad,
                             kCellH - 2 * kCellPad, kCellRadius, EPD_BLACK);
    }
    uint8_t color = sel ? EPD_WHITE : EPD_BLACK;

    if (i < kCharCount) {
      char buf[2] = {(caseUpper_ ? kUpper : kLower)[i], '\0'};
      int tw = Canvas::textWidth(buf, FONT_EMPHASIS);
      canvas_.drawText(x + (kCellW - tw) / 2, y + (kCellH - FONT_EMPHASIS.height) / 2, buf, color,
                        FONT_EMPHASIS);
    } else {
      const char *label = i == kCase ? (caseUpper_ ? "ABC" : "abc")
                           : i == kSpace ? "_"
                           : i == kDel   ? "<-"
                           : i == kOk    ? "OK"
                                         : "X";
      int tw = Canvas::textWidth(label, FONT_BODY);
      canvas_.drawText(x + (kCellW - tw) / 2, y + (kCellH - FONT_BODY.height) / 2, label, color,
                        FONT_BODY);
    }
  }

  canvas_.drawFastHLine(10, 178, 180, EPD_BLACK);
  canvas_.drawText(10, 181, "BOOT insere | PWR nav/linha", EPD_BLACK, FONT_BODY);
  epd_.displayPart();
}
