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
  enum class Error : uint8_t { None, InvalidSize, Allocation, Gpio, Spi, BusyTimeout };
  static constexpr uint32_t kBusyTimeoutMs = 5000;

  EPaperDisplay(int width, int height, const EPaperPins &pins);

  bool init();
  void clear();
  bool display();

  bool initPartial();
  bool displayPartBaseImage();
  bool displayPart();

  void drawPixel(uint16_t x, uint16_t y, uint8_t color);

  // Erros sao latched ate reboot; antes de init, healthy=true e ready=false.
  bool ready() const { return ready_; }
  bool healthy() const { return error_ == Error::None; }
  Error error() const { return error_; }
  // Call with no display transaction active after removing panel power.
  void invalidatePower() { ready_ = false; shadowValid_ = false; }

  // Canvas usa acesso direto: sem init valido, area vazia e ponteiro nao nulo
  // tornam clear/memcpy de tamanho zero e clipping seguros, sem inicializar HW.
  int width() const { return ready() ? width_ : 0; }
  int height() const { return ready() ? height_ : 0; }
  uint8_t *buffer() { return ready() ? buffer_ : &discard_; }
  int bufferLen() const { return ready() ? bufferLen_ : 0; }

private:
  const EPaperPins pins_;
  const int width_;
  const int height_;
  const int bufferLen_;
  spi_device_handle_t spi_ = nullptr;
  uint8_t *buffer_ = nullptr;
  uint8_t discard_ = 0;
  bool ready_ = false;
  bool shadowValid_ = false;
  Error error_ = Error::None;

  // Ultimo frame de fato enviado ao painel - displayPart() compara
  // buffer_ contra isto e pula o refresh inteiro quando nada mudou.
  // Perdido a cada boot/wake/reset do controlador: shadowValid_ impede
  // pular o primeiro refresh, mesmo se o frame coincidir com RAM zerada.
  uint8_t *shadow_ = nullptr;

  bool begin();
  bool check(esp_err_t result, Error error);
  bool fail(Error error);
  bool spiGpioInit();
  bool spiPortInit();
  bool readBusy();
  bool reset();

  bool setCs(bool level) { return check(gpio_set_level((gpio_num_t)pins_.cs, level), Error::Gpio); }
  bool setDc(bool level) { return check(gpio_set_level((gpio_num_t)pins_.dc, level), Error::Gpio); }
  bool setRst(bool level) { return check(gpio_set_level((gpio_num_t)pins_.rst, level), Error::Gpio); }

  void spiSendByte(uint8_t data);
  void sendData(uint8_t data);
  void sendCommand(uint8_t command);
  void writeBytes(const uint8_t *data, int len);

  void setWindows(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd);
  void setCursor(uint16_t xStart, uint16_t yStart);
  bool setLut(const uint8_t *lut);
  bool turnOnDisplay();
  bool turnOnDisplayPart();
};
