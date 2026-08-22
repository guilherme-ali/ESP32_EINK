#include "epaper.h"
#include "esp_heap_caps.h"
#include <string.h>

// LUTs de waveform do SSD1681 para o painel 1.54" - valores do vendor,
// nao alterar sem redocumentar a origem.
static const uint8_t WF_FULL_1IN54[159] = {
    0x80, 0x48, 0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x40, 0x48, 0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x80, 0x48, 0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x40, 0x48, 0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0xA,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x8,  0x1,  0x0,  0x8,  0x1,  0x0,  0x2,
    0xA,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0,  0x0,  0x0,
    0x22, 0x17, 0x41, 0x0,  0x32, 0x20};

static const uint8_t WF_PARTIAL_1IN54_0[159] = {
    0x0,  0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x80, 0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x40, 0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x80, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0xF,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x1,  0x1,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0,  0x0,  0x0,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28};

EPaperDisplay::EPaperDisplay(int width, int height, const EPaperPins &pins)
    : pins_(pins), width_(width), height_(height), bufferLen_(width * height / 8) {
  spiPortInit();
  spiGpioInit();
  // RAM interna (nao PSRAM): sao so 5000 bytes e o buffer leva milhares
  // de read-modify-write por tela desenhada (Canvas::drawPixel) - RAM
  // interna e bem mais rapida pra isso que PSRAM, e sobra espaco (o
  // build usa ~22% dos 320KB). MALLOC_CAP_DMA garante que o SPI consiga
  // fazer DMA direto do buffer sem copia intermediaria.
  buffer_ = (uint8_t *)heap_caps_malloc(bufferLen_, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  assert(buffer_);
  shadow_ = (uint8_t *)heap_caps_malloc(bufferLen_, MALLOC_CAP_INTERNAL);
  assert(shadow_);
  memset(shadow_, 0, bufferLen_);
}

void EPaperDisplay::spiGpioInit() {
  gpio_config_t conf = {};
  conf.intr_type = GPIO_INTR_DISABLE;
  conf.mode = GPIO_MODE_OUTPUT;
  conf.pin_bit_mask = (1ULL << pins_.rst) | (1ULL << pins_.dc) | (1ULL << pins_.cs);
  conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&conf));

  conf.mode = GPIO_MODE_INPUT;
  conf.pin_bit_mask = (1ULL << pins_.busy);
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&conf));

  setRst(true);
}

void EPaperDisplay::spiPortInit() {
  spi_bus_config_t busCfg = {};
  busCfg.miso_io_num = -1;
  busCfg.mosi_io_num = pins_.mosi;
  busCfg.sclk_io_num = pins_.sck;
  busCfg.quadwp_io_num = -1;
  busCfg.quadhd_io_num = -1;
  busCfg.max_transfer_sz = width_ * height_;

  spi_device_interface_config_t devCfg = {};
  devCfg.spics_io_num = -1;
  devCfg.clock_speed_hz = 40 * 1000 * 1000;
  devCfg.mode = 0;
  devCfg.queue_size = 7;

  ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)pins_.spiHost, &busCfg, SPI_DMA_CH_AUTO));
  ESP_ERROR_CHECK(spi_bus_add_device((spi_host_device_t)pins_.spiHost, &devCfg, &spi_));
}

void EPaperDisplay::readBusy() {
  while (gpio_get_level((gpio_num_t)pins_.busy) == 1) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void EPaperDisplay::spiSendByte(uint8_t data) {
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 8;
  t.tx_buffer = &data;
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
}

void EPaperDisplay::sendData(uint8_t data) {
  setDc(true);
  setCs(false);
  spiSendByte(data);
  setCs(true);
}

void EPaperDisplay::sendCommand(uint8_t command) {
  setDc(false);
  setCs(false);
  spiSendByte(command);
  setCs(true);
}

void EPaperDisplay::writeBytes(const uint8_t *data, int len) {
  setDc(true);
  setCs(false);
  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 8 * len;
  t.tx_buffer = data;
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
  setCs(true);
}

void EPaperDisplay::setWindows(uint16_t xStart, uint16_t yStart, uint16_t xEnd, uint16_t yEnd) {
  sendCommand(0x44);
  sendData((xStart >> 3) & 0xFF);
  sendData((xEnd >> 3) & 0xFF);

  sendCommand(0x45);
  sendData(yStart & 0xFF);
  sendData((yStart >> 8) & 0xFF);
  sendData(yEnd & 0xFF);
  sendData((yEnd >> 8) & 0xFF);
}

void EPaperDisplay::setCursor(uint16_t xStart, uint16_t yStart) {
  sendCommand(0x4E);
  sendData(xStart & 0xFF);

  sendCommand(0x4F);
  sendData(yStart & 0xFF);
  sendData((yStart >> 8) & 0xFF);
}

void EPaperDisplay::setLut(const uint8_t *lut) {
  sendCommand(0x32);
  writeBytes(lut, 153);
  readBusy();

  sendCommand(0x3F);
  sendData(lut[153]);

  sendCommand(0x03);
  sendData(lut[154]);

  sendCommand(0x04);
  sendData(lut[155]);
  sendData(lut[156]);
  sendData(lut[157]);

  sendCommand(0x2C);
  sendData(lut[158]);
}

void EPaperDisplay::turnOnDisplay() {
  sendCommand(0x22);
  sendData(0xC7);
  sendCommand(0x20);
  readBusy();
}

void EPaperDisplay::turnOnDisplayPart() {
  sendCommand(0x22);
  sendData(0xCF);
  sendCommand(0x20);
  readBusy();
}

void EPaperDisplay::init() {
  setRst(true);
  vTaskDelay(pdMS_TO_TICKS(50));
  setRst(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  setRst(true);
  vTaskDelay(pdMS_TO_TICKS(50));

  readBusy();
  sendCommand(0x12); // SWRESET
  readBusy();

  sendCommand(0x01); // driver output control
  sendData(0xC7);
  sendData(0x00);
  sendData(0x01);

  sendCommand(0x11); // data entry mode
  sendData(0x01);

  setWindows(0, height_ - 1, width_ - 1, 0);

  sendCommand(0x3C); // border waveform
  sendData(0x01);

  sendCommand(0x18);
  sendData(0x80);

  sendCommand(0x22); // load temperature and waveform setting
  sendData(0xB1);
  sendCommand(0x20);

  setCursor(0, height_ - 1);
  readBusy();

  setLut(WF_FULL_1IN54);
}

void EPaperDisplay::clear() {
  memset(buffer_, 0xFF, bufferLen_);
}

void EPaperDisplay::display() {
  sendCommand(0x24);
  writeBytes(buffer_, bufferLen_);
  turnOnDisplay();
  memcpy(shadow_, buffer_, bufferLen_);
}

void EPaperDisplay::displayPartBaseImage() {
  sendCommand(0x24);
  writeBytes(buffer_, bufferLen_);
  sendCommand(0x26);
  writeBytes(buffer_, bufferLen_);
  turnOnDisplay();
  memcpy(shadow_, buffer_, bufferLen_);
}

void EPaperDisplay::initPartial() {
  setRst(true);
  vTaskDelay(pdMS_TO_TICKS(50));
  setRst(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  setRst(true);
  vTaskDelay(pdMS_TO_TICKS(50));

  readBusy();
  setLut(WF_PARTIAL_1IN54_0);

  sendCommand(0x37);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);
  sendData(0x40);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);
  sendData(0x00);

  sendCommand(0x3C);
  sendData(0x80);

  sendCommand(0x22);
  sendData(0xC0);
  sendCommand(0x20);
  readBusy();
}

// A janela reduzida (setWindows/setCursor por faixa de linhas) foi
// tentada e revertida: duas convencoes de mapeamento Y foram testadas
// na placa (direta e invertida, essa ultima batendo com o data entry
// mode 0x01 usado em init()) e as duas corromperam a tela - sem a
// documentacao exata do SSD1681 em maos, virou tentativa as cegas.
// Fica so o pulo do refresh quando nada mudou desde o ultimo frame
// (ver shadow_ abaixo) - sem risco visual, ainda um ganho real quando
// uma tecla nao muda nada (ex: apertar BOOT numa tela ja atualizada).
void EPaperDisplay::displayPart() {
  if (memcmp(buffer_, shadow_, bufferLen_) == 0) return; // identico ao ultimo frame - sem refresh

  sendCommand(0x24);
  writeBytes(buffer_, bufferLen_);
  turnOnDisplayPart();
  memcpy(shadow_, buffer_, bufferLen_);
}

void EPaperDisplay::drawPixel(uint16_t x, uint16_t y, uint8_t color) {
  if (x >= (uint16_t)width_ || y >= (uint16_t)height_) return;
  uint16_t index = y * (width_ / 8) + (x >> 3);
  uint8_t bit = 7 - (x & 0x07);
  if (color == EPD_WHITE) {
    buffer_[index] |= (0x01 << bit);
  } else {
    buffer_[index] &= ~(0x01 << bit);
  }
}
