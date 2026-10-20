#include "../../src/display/epaper.h"
#include "../../src/ui/canvas.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef NDEBUG
#error These tests require assertions to be enabled.
#endif

namespace {
constexpr EPaperPins pins{1, 2, 3, 4, 5, 6, 2};
struct Allocation { uint8_t *base; size_t size; };
std::vector<Allocation> allocations;
struct Fake {
  uint32_t now = 0;
  int mallocs = 0, frees = 0, gpio = 0, tx = 0, delays = 0, reads = 0;
  int buses = 0, devices = 0, busFrees = 0;
  int failMalloc = 0, failGpio = 0, failTx = 0, failBus = 0, failDevice = 0;
  int busyFromRead = 0, busyPolls = 0, txAtBusy = 0;
  bool dc = false, cs = true, failed = false;
  std::vector<uint8_t> commands;
} fake;

void canaries() {
  for (const auto &allocation : allocations) {
    assert(allocation.base[0] == 0xA5);
    assert(allocation.base[allocation.size + 1] == 0x5A);
  }
}

void resetFake() {
  canaries();
  for (const auto &allocation : allocations) std::free(allocation.base);
  allocations.clear();
  fake = Fake{};
}

void canvasCalls(EPaperDisplay &epd) {
  Canvas canvas(epd);
  canvas.clear(EPD_BLACK);
  canvas.clear();
  canvas.drawFastHLine(-4, 100, 208, EPD_BLACK);
  canvas.fillRect(-8, -8, 216, 216, EPD_BLACK);
  canvas.drawPixel(-1, 0, EPD_BLACK);
  epd.drawPixel(UINT16_MAX, UINT16_MAX, EPD_BLACK);
  epd.clear();
}

void noMoreWork(EPaperDisplay &epd) {
  const auto before = fake;
  for (int i = 0; i < 3; ++i) {
    canvasCalls(epd);
    assert(!epd.init());
    assert(!epd.initPartial());
    assert(!epd.display());
    assert(!epd.displayPart());
    assert(!epd.displayPartBaseImage());
    assert(!epd.ready() && !epd.healthy());
    assert(epd.width() == 0 && epd.height() == 0 && epd.bufferLen() == 0);
    assert(epd.buffer() != nullptr);
  }
  assert(fake.mallocs == before.mallocs && fake.frees == before.frees);
  assert(fake.gpio == before.gpio && fake.tx == before.tx);
  assert(fake.delays == before.delays && fake.reads == before.reads);
  assert(fake.buses == before.buses && fake.devices == before.devices);
  canaries();
}

void constructionAndSuccess() {
  resetFake();
  EPaperDisplay epd(200, 200, pins);
  assert(!epd.ready() && epd.healthy() && epd.error() == EPaperDisplay::Error::None);
  canvasCalls(epd);
  assert(!epd.initPartial() && !epd.display() && !epd.displayPartBaseImage() && !epd.displayPart());
  assert(fake.mallocs == 0 && fake.gpio == 0 && fake.buses == 0);
  assert(fake.tx == 0 && fake.delays == 0 && fake.reads == 0);
  assert(epd.init());
  assert(epd.ready() && epd.healthy());
  assert(fake.mallocs == 2 && fake.buses == 1 && fake.devices == 1);
  assert(epd.width() == 200 && epd.height() == 200 && epd.bufferLen() == 5000);
  for (int i = 0; i < epd.bufferLen(); ++i) assert(epd.buffer()[i] == 0xFF);
  assert(std::find(fake.commands.begin(), fake.commands.end(), 0x24) == fake.commands.end());
  assert(epd.displayPartBaseImage());
  assert(epd.initPartial());
  int tx = fake.tx;
  assert(epd.displayPart() && fake.tx == tx); // Confirmed base image is unchanged.
  epd.drawPixel(0, 0, EPD_BLACK);
  epd.drawPixel(199, 199, EPD_BLACK);
  assert(epd.buffer()[0] == 0x7F && epd.buffer()[4999] == 0xFE);
  assert(epd.displayPart() && fake.tx > tx);
  tx = fake.tx;
  assert(epd.displayPart() && fake.tx == tx);
  canvasCalls(epd);
  assert(fake.tx == tx); // Drawing never refreshes/reinitializes hardware.
  assert(epd.display());
  uint8_t *buffer = epd.buffer();
  assert(epd.init()); // Restore full LUT for lockscreen without reallocating SPI/RAM.
  assert(epd.buffer() == buffer && fake.mallocs == 2 && fake.buses == 1);
  assert(epd.displayPart()); // No valid shadow following controller reset.
  epd.clear();
  std::memset(epd.buffer(), 0, epd.bufferLen());
  assert(epd.init() && epd.displayPart()); // First all-black frame is not mistaken for shadow.
  canaries();
}

void allocationAndSetupFailures() {
  for (int failure = 1; failure <= 2; ++failure) {
    resetFake();
    fake.failMalloc = failure;
    EPaperDisplay epd(200, 200, pins);
    assert(!epd.init() && epd.error() == EPaperDisplay::Error::Allocation);
    assert(allocations.empty() && fake.buses == 0 && fake.gpio == 0);
    noMoreWork(epd);
  }
  for (int failure = 1; failure <= 5; ++failure) {
    resetFake();
    fake.failGpio = failure;
    EPaperDisplay epd(200, 200, pins);
    assert(!epd.init() && epd.error() == EPaperDisplay::Error::Gpio);
    assert(allocations.empty() && fake.buses == 0 && fake.delays == 0);
    noMoreWork(epd);
  }
  for (int failure = 0; failure < 2; ++failure) {
    resetFake();
    fake.failBus = !failure;
    fake.failDevice = failure;
    EPaperDisplay epd(200, 200, pins);
    assert(!epd.init() && epd.error() == EPaperDisplay::Error::Spi);
    assert(fake.busFrees == failure && allocations.empty());
    noMoreWork(epd);
  }
  for (const int width : {-1, 0, 199, INT32_MAX}) {
    resetFake();
    EPaperDisplay epd(width, 200, pins);
    assert(!epd.init() && epd.error() == EPaperDisplay::Error::InvalidSize);
    assert(fake.mallocs == 0 && fake.gpio == 0);
    noMoreWork(epd);
  }
}

using Operation = bool (EPaperDisplay::*)();
void runtimeFailures() {
  const Operation operations[] = {&EPaperDisplay::init, &EPaperDisplay::initPartial,
    &EPaperDisplay::displayPartBaseImage, &EPaperDisplay::displayPart, &EPaperDisplay::display};
  for (Operation operation : operations) {
    resetFake();
    EPaperDisplay reference(200, 200, pins);
    assert(reference.init());
    int start = fake.tx;
    assert((reference.*operation)());
    int transactions = fake.tx - start;
    for (int failure = 1; failure <= transactions; ++failure) {
      resetFake();
      EPaperDisplay epd(200, 200, pins);
      assert(epd.init());
      fake.failTx = fake.tx + failure;
      assert(!(epd.*operation)());
      assert(epd.error() == EPaperDisplay::Error::Spi && fake.tx == fake.failTx);
      assert(fake.cs); // Deassert CS even if the transfer failed.
      noMoreWork(epd);
    }
    resetFake();
    EPaperDisplay gpioReference(200, 200, pins);
    assert(gpioReference.init());
    const int startGpio = fake.gpio;
    assert((gpioReference.*operation)());
    const int gpioOperations = fake.gpio - startGpio;
    for (int failure = 1; failure <= gpioOperations; ++failure) {
      resetFake();
      EPaperDisplay epd(200, 200, pins);
      assert(epd.init());
      fake.failGpio = fake.gpio + failure;
      assert(!(epd.*operation)());
      assert(epd.error() == EPaperDisplay::Error::Gpio && fake.gpio == fake.failGpio);
      noMoreWork(epd);
    }
    // Each BUSY wait within the operation is independently forced stuck.
    resetFake();
    EPaperDisplay busyReference(200, 200, pins);
    assert(busyReference.init());
    int startReads = fake.reads;
    assert((busyReference.*operation)());
    int waits = fake.reads - startReads;
    for (int wait = 1; wait <= waits; ++wait) {
      resetFake();
      EPaperDisplay epd(200, 200, pins);
      assert(epd.init());
      fake.busyFromRead = fake.reads + wait;
      fake.now = UINT32_MAX - 1000; // Timeout remains correct across millis wrap.
      const uint32_t started = fake.now;
      assert(!(epd.*operation)());
      assert(epd.error() == EPaperDisplay::Error::BusyTimeout);
      assert(uint32_t(fake.now - started) >= EPaperDisplay::kBusyTimeoutMs);
      assert(uint32_t(fake.now - started) <= EPaperDisplay::kBusyTimeoutMs + 120);
      assert(fake.busyPolls >= 500 && fake.busyPolls <= 501);
      assert(fake.tx == fake.txAtBusy); // No commands are sent after BUSY times out.
      noMoreWork(epd);
    }
  }
  resetFake();
  EPaperDisplay neverReady(200, 200, pins);
  fake.busyFromRead = 1;
  assert(!neverReady.init() && fake.tx == 0 && fake.now == 5120);
  noMoreWork(neverReady);
}
} // namespace

uint32_t millis() { return fake.now; }
void vTaskDelay(TickType_t ticks) {
  assert(ticks > 0 && !fake.failed);
  ++fake.delays;
  fake.now += ticks * 10;
}
void *heap_caps_malloc(size_t size, uint32_t caps) {
  if (++fake.mallocs == fake.failMalloc) return nullptr;
  assert(caps & MALLOC_CAP_INTERNAL);
  auto *base = static_cast<uint8_t *>(std::malloc(size + 2));
  assert(base);
  base[0] = 0xA5;
  base[size + 1] = 0x5A;
  allocations.push_back({base, size});
  return base + 1;
}
void heap_caps_free(void *ptr) {
  ++fake.frees;
  auto it = std::find_if(allocations.begin(), allocations.end(),
    [ptr](const Allocation &allocation) { return allocation.base + 1 == ptr; });
  assert(it != allocations.end());
  canaries();
  std::free(it->base);
  allocations.erase(it);
}
esp_err_t gpio_config(const gpio_config_t *) {
  fake.failed = ++fake.gpio == fake.failGpio;
  return fake.failed ? -1 : ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  if (++fake.gpio == fake.failGpio) {
    fake.failed = true;
    return -1;
  }
  if (pin == pins.dc) fake.dc = level;
  if (pin == pins.cs) fake.cs = level;
  return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) {
  assert(pin == pins.busy);
  ++fake.reads;
  bool busy = fake.busyFromRead && fake.reads >= fake.busyFromRead;
  if (busy && ++fake.busyPolls == 1) fake.txAtBusy = fake.tx;
  return busy;
}
esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t *config, int) {
  ++fake.buses;
  assert(config->mosi_io_num == pins.mosi && config->sclk_io_num == pins.sck);
  return fake.failBus ? -1 : ESP_OK;
}
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *device) {
  ++fake.devices;
  if (fake.failDevice) return -1;
  *device = &fake;
  return ESP_OK;
}
esp_err_t spi_bus_free(spi_host_device_t) { ++fake.busFrees; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction) {
  assert(device && transaction->tx_buffer && !fake.cs && transaction->length > 0 && !fake.failed);
  ++fake.tx;
  if (!fake.dc) fake.commands.push_back(*static_cast<const uint8_t *>(transaction->tx_buffer));
  fake.failed = fake.tx == fake.failTx;
  return fake.failed ? -1 : ESP_OK;
}

int main() {
  constructionAndSuccess();
  allocationAndSetupFailures();
  runtimeFailures();
  resetFake();
  std::puts("EPaper real driver + Canvas: constructor, clipping, allocation/GPIO/SPI failures, BUSY timeout/wrap, sticky failure and shadow OK");
}
