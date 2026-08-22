#pragma once
// Driver do painel e-paper SSD1681 200x200 (Waveshare ESP32-S3-ePaper-1.54).
// Portado quase literalmente de waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/Arduino/08_Audio_Test/src/display/epaper_driver_bsp.{h,cpp}
// (sequencia de registradores e LUTs de waveform sao especificas do
// controlador e nao devem ser reescritas).

#include <Arduino.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

enum EPaperColor : uint8_t {
  EPD_WHITE = 0xFF,
  EPD_BLACK = 0x00,
};

struct EPaperPins {
  uint8_t cs;
  uint8_t dc;
  uint8_t rst;
  uint8_t busy;
  uint8_t mosi;
  uint8_t sck;
  int spiHost;
};

class EPaperDisplay {
public:
  EPaperDisplay(int width, int height, const EPaperPins &pins);

  void init();
  void clear();
  void display();

  void initPartial();
  void displayPartBaseImage();
  void displayPart();

  void drawPixel(uint16_t x, uint16_t y, uint8_t color);

  int width() const { return width_; }
  int height() const { return height_; }
  uint8_t *buffer() { return buffer_; }
  int bufferLen() const { return bufferLen_; }

private:
  const EPaperPins pins_;
  const int width_;
  const int height_;
  const int bufferLen_;
  spi_device_handle_t spi_ = nullptr;
  uint8_t *buffer_ = nullptr;

  void spiGpioInit();
  void spiPortInit();
  void readBusy();

  void setCs(bool level) { gpio_set_level((gpio_num_t)pins_.cs, level); }
  void setDc(bool level) { gpio_set_level((gpio_num_t)pins_.dc, level); }
  void setRst(bool level) { gpio_set_level((gpio_num_t)pins_.rst, level); }

  void spiSendByte(uint8_t data);
  void sendData(uint8_t data);
  void sendCommand(uint8_t command);
  void writeBytes(const uint8_t *data, int len);

  void setWindows(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd);
  void setCursor(uint16_t xStart, uint16_t yStart);
  void setLut(const uint8_t *lut);
  void turnOnDisplay();
  void turnOnDisplayPart();
};
