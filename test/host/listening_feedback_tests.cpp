#include "../../src/ui/listening_feedback.h"
#include "../../src/ui/screens.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (0)
using State = ListeningFeedback::State;
using Clock = std::chrono::steady_clock;

struct FeedbackTask {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  uint32_t notifications = 0;
  std::atomic<bool> deleted{false};
};

namespace {
thread_local FeedbackTask *currentTask = nullptr;
struct TaskExit {};
std::vector<std::unique_ptr<FeedbackTask>> tasks;
std::atomic<bool> pauseDispatch{false};
bool failTask = false;
unsigned taskCreations = 0;
std::atomic<unsigned> yields{0}, diagnostics{0};
std::atomic<unsigned> notifications{0};
std::atomic<uint32_t> ticks{0};
std::atomic<ListeningFeedback *> observed{nullptr};
const EPaperPins pins = {1, 2, 3, 4, 5, 6, 0};

enum class Stage { None, Init, Full, PartialInit, Partial };
struct Hardware {
  std::mutex mutex;
  std::condition_variable cv;
  Stage hold = Stage::None, entered = Stage::None, fail = Stage::None;
  bool released = false, busyStuck = false, dc = false;
  uint8_t command = 0, update = 0;
  unsigned io = 0, allocations = 0, power = 0, init = 0, full = 0, partialInit = 0, partial = 0;
  unsigned failAllocationAt = 0;
  std::vector<uint8_t> ram24, ram26;
  std::vector<std::vector<uint8_t>> frames;
  std::vector<uint8_t *> allocated;
} hw;

void waitFor(const std::function<bool()> &predicate) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    CHECK(Clock::now() < deadline);
    std::this_thread::yield();
  }
}

void assertOwnership() {
  auto *feedback = observed.load(std::memory_order_acquire);
  if (feedback) {
    CHECK(currentTask != nullptr);
    CHECK(feedback->busy());
  }
}

void resetHardware() {
  observed.store(nullptr);
  for (auto &task : tasks) {
    CHECK(task->thread.joinable());
    task->thread.join();
    CHECK(task->deleted);
  }
  tasks.clear();
  for (auto *allocation : hw.allocated) {
    for (int n = 0; n < 16; ++n) CHECK(allocation[n] == 0xA5 && allocation[5016 + n] == 0xA5);
    std::free(allocation);
  }
  hw.allocated.clear();
  hw.hold = hw.entered = hw.fail = Stage::None;
  hw.released = hw.busyStuck = hw.dc = false;
  hw.command = hw.update = 0;
  hw.io = hw.allocations = hw.power = hw.init = hw.full = hw.partialInit = hw.partial = 0;
  hw.failAllocationAt = 0;
  hw.ram24.clear();
  hw.ram26.clear();
  hw.frames.clear();
  pauseDispatch = false;
  failTask = false;
}

void unpause() {
  pauseDispatch = false;
  for (auto &task : tasks) {
    std::lock_guard<std::mutex> lock(task->mutex);
    task->cv.notify_all();
  }
}

void waitStage(Stage stage) {
  std::unique_lock<std::mutex> lock(hw.mutex);
  CHECK(hw.cv.wait_for(lock, std::chrono::seconds(5), [stage] { return hw.entered == stage; }));
}

void releaseStage() {
  std::lock_guard<std::mutex> lock(hw.mutex);
  hw.released = true;
  hw.cv.notify_all();
}

void powerOn() {
  assertOwnership();
  std::lock_guard<std::mutex> lock(hw.mutex);
  ++hw.power;
}

std::vector<uint8_t> expectedFrame(State state) {
  observed.store(nullptr);
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  CHECK(epd.init());
  if (state == State::Listening) Screens::paintVoiceState(canvas, "Ouvindo", "Pode falar", true);
  else if (state == State::Thinking) Screens::paintVoiceState(canvas, "Pensando", "Aguarde um instante", false);
  else Screens::paintVoiceState(canvas, "Falando", "PWR cancela", false);
  return std::vector<uint8_t>(epd.buffer(), epd.buffer() + epd.bufferLen());
}

void renderer() {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  Screens::paintVoiceState(canvas, nullptr, nullptr, true);
  CHECK(hw.io == 0 && hw.allocations == 0);
  CHECK(epd.init());
  const unsigned io = hw.io;
  for (const char *title : {"Ouvindo", "Pensando", "Falando"}) {
    const int width = Canvas::textWidth(title, FONT_CLOCK);
    CHECK(FONT_CLOCK.height == 40 && width <= 180);
    Screens::paintVoiceState(canvas, title, "Pode falar", true);
    CHECK(hw.io == io); // Renderer nao atualiza hardware.
    bool ink = false;
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x) {
      const bool black = !(epd.buffer()[y * 25 + x / 8] & (0x80 >> (x % 8)));
      if (black) {
        ink = true;
        CHECK(x >= 10 && x < 190 && y >= 38 && y < 173);
        if (y >= 104 && y < 144) CHECK(x >= (200 - width) / 2 && x < (200 + width) / 2 + 1);
      }
    }
    CHECK(ink);
  }
  CHECK(Canvas::textWidth("Ouvindo", FONT_CLOCK) == 115);
  Screens::paintVoiceState(canvas, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW\nBAD",
                           "hint longa longa longa longa longa longa longa\nBAD", false);
  for (int y = 0; y < 200; ++y) for (int x = 0; x < 200; ++x)
    if (!(epd.buffer()[y * 25 + x / 8] & (0x80 >> (x % 8))))
      CHECK(x >= 10 && x < 190 && y < 173);
  Screens::paintVoiceState(canvas, nullptr, nullptr, false);
  for (int y = 104; y < 200; ++y) for (int x = 0; x < 25; ++x) CHECK(epd.buffer()[y * 25 + x] == 0xFF);
}

void beginAndCancel() {
  EPaperDisplay epd(200, 200, pins), other(200, 200, pins);
  Canvas canvas(epd);
  ListeningFeedback feedback;
  CHECK(!feedback.request(State::Listening));
  CHECK(!feedback.busy());
  CHECK(!feedback.begin(epd, canvas, nullptr));
  failTask = true;
  CHECK(!feedback.begin(epd, canvas, powerOn));
  CHECK(!feedback.busy() && !feedback.ready());
  failTask = false;
  CHECK(feedback.begin(epd, canvas, powerOn));
  const unsigned created = taskCreations;
  CHECK(feedback.begin(epd, canvas, powerOn));
  CHECK(!feedback.begin(other, canvas, powerOn));
  CHECK(!feedback.request(static_cast<State>(42)));
  CHECK(hw.io == 0 && hw.power == 0 && hw.allocations == 0 && taskCreations == created);
  pauseDispatch = true;
  observed.store(&feedback);
  for (int n = 0; n < 1000; ++n) CHECK(feedback.request(static_cast<State>(n % 3)));
  CHECK(feedback.busy());
  feedback.finish();
  CHECK(!feedback.busy());
  CHECK(hw.io == 0 && hw.power == 0 && hw.allocations == 0 && taskCreations == created);
  unpause();
  CHECK(feedback.request(State::Listening));
  waitFor([&] { return !feedback.busy(); });
  CHECK(feedback.ready() && !feedback.failed());
  CHECK(hw.full == 1 && hw.partialInit == 1 && hw.partial == 0 && hw.init == 1);
}

void firstFullAndCoalesce(const std::vector<uint8_t> &listening, const std::vector<uint8_t> &speaking) {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  ListeningFeedback feedback;
  CHECK(feedback.begin(epd, canvas, powerOn));
  observed.store(&feedback);
  hw.hold = Stage::Full;
  CHECK(feedback.request(State::Listening));
  waitStage(Stage::Full);
  CHECK(feedback.busy() && !feedback.ready());
  const unsigned created = taskCreations;
  const unsigned notified = notifications;
  for (int n = 0; n < 1000; ++n) CHECK(feedback.request(State::Listening));
  CHECK(notifications == notified); // Mesmo estado em voo nao muda ticket.
  for (int n = 0; n < 1000; ++n) CHECK(feedback.request(State::Thinking));
  CHECK(feedback.request(State::Speaking));
  CHECK(taskCreations == created && hw.allocations == 2);
  releaseStage();
  waitFor([&] { return !feedback.busy(); });
  CHECK(feedback.ready() && !feedback.failed());
  CHECK(hw.frames.size() == 2 && hw.frames[0] == listening && hw.frames[1] == speaking);
  CHECK(hw.ram26 == listening && hw.full == 1 && hw.partialInit == 1 && hw.partial == 1);
  const unsigned beforeIo = hw.io, beforePower = hw.power, beforeNotify = notifications;
  CHECK(feedback.request(State::Speaking));
  waitFor([&] { return !feedback.busy(); });
  CHECK(hw.io == beforeIo && hw.power == beforePower && notifications == beforeNotify);
  CHECK(hw.partial == 1); // Shadow real suprime refresh identico.
}

void finishDuring(Stage held, const std::vector<uint8_t> &listening) {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  ListeningFeedback feedback;
  CHECK(feedback.begin(epd, canvas, powerOn));
  observed.store(&feedback);
  hw.hold = held;
  CHECK(feedback.request(State::Listening));
  waitStage(held);
  CHECK(feedback.request(State::Thinking));
  CHECK(feedback.request(State::Speaking));
  const unsigned yieldBefore = yields;
  std::atomic<bool> returned{false};
  std::thread finisher([&] { feedback.finish(); returned = true; });
  waitFor([&] { return yields.load() > yieldBefore + 100; });
  CHECK(!returned && feedback.busy());
  releaseStage();
  finisher.join();
  CHECK(returned && !feedback.busy() && feedback.ready());
  CHECK(hw.frames.size() == 1 && hw.frames[0] == listening);
  CHECK(hw.partialInit == 1); // finish nao retorna antes do cleanup/LUT.
  observed.store(nullptr);
  Screens::paintVoiceState(canvas, "legado", "owner UI", false);
  const std::vector<uint8_t> legacy(epd.buffer(), epd.buffer() + 5000);
  const unsigned before = hw.io;
  for (int n = 0; n < 1000; ++n) std::this_thread::yield();
  CHECK(hw.io == before && std::equal(legacy.begin(), legacy.end(), epd.buffer()));
  observed.store(&feedback);
  CHECK(feedback.request(State::Speaking));
  waitFor([&] { return !feedback.busy(); });
  CHECK(hw.partial == 1);
}

void errorSticky(Stage fail, bool busyTimeout = false) {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  ListeningFeedback feedback;
  CHECK(feedback.begin(epd, canvas, powerOn));
  observed.store(&feedback);
  if (fail == Stage::Partial) {
    CHECK(feedback.request(State::Listening));
    waitFor([&] { return !feedback.busy(); });
    CHECK(feedback.ready());
  }
  hw.fail = fail;
  hw.busyStuck = busyTimeout;
  const unsigned logs = diagnostics;
  CHECK(feedback.request(State::Thinking));
  waitFor([&] { return !feedback.busy(); });
  CHECK(!feedback.ready() && feedback.failed());
  CHECK(feedback.error() == (busyTimeout ? EPaperDisplay::Error::BusyTimeout : EPaperDisplay::Error::Spi));
  const unsigned before = hw.io, allocated = hw.allocations;
  CHECK(!feedback.request(State::Listening));
  feedback.finish();
  CHECK(hw.io == before && hw.allocations == allocated);
#if ARDUINO_USB_CDC_ON_BOOT
  CHECK(diagnostics == logs + 1);
#else
  CHECK(diagnostics == logs);
#endif
}

void requestClaimStress() {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  ListeningFeedback feedback;
  CHECK(feedback.begin(epd, canvas, powerOn));
  observed.store(&feedback);
  for (int n = 0; n < 500; ++n) {
    CHECK(feedback.request(static_cast<State>(n % 3)));
    if (n % 7 == 0) feedback.finish();
    else std::this_thread::yield();
  }
  CHECK(feedback.request(State::Speaking));
  waitFor([&] { return !feedback.busy(); });
  CHECK(feedback.ready() && !feedback.failed());
  CHECK(hw.allocations == 2 && hw.full == 1 && hw.init == 1);
}

void alreadyReadyAndPartialFinish(const std::vector<uint8_t> &listening) {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  CHECK(epd.init());
  Screens::paintVoiceState(canvas, "legado", "UI", false);
  CHECK(epd.displayPartBaseImage() && epd.initPartial());
  ListeningFeedback feedback;
  CHECK(feedback.begin(epd, canvas, powerOn));
  observed.store(&feedback);
  hw.hold = Stage::Partial;
  CHECK(feedback.request(State::Listening));
  waitStage(Stage::Partial);
  CHECK(feedback.busy() && !feedback.ready());
  CHECK(feedback.request(State::Thinking));
  std::atomic<bool> returned{false};
  const unsigned before = yields;
  std::thread finisher([&] { feedback.finish(); returned = true; });
  waitFor([&] { return yields.load() > before + 100; });
  CHECK(!returned && feedback.busy());
  releaseStage();
  finisher.join();
  CHECK(feedback.ready() && !feedback.busy());
  CHECK(hw.init == 1 && hw.full == 1 && hw.partialInit == 1 && hw.partial == 1);
  CHECK(hw.frames.size() == 2 && hw.frames.back() == listening);
  const unsigned notified = notifications, power = hw.power;
  // finish invalida a deduplicacao; mesmo estado pode ser pedido novamente.
  CHECK(feedback.request(State::Listening));
  waitFor([&] { return !feedback.busy(); });
  CHECK(notifications == notified + 1 && hw.power == power + 1);
  CHECK(hw.partial == 1); // Shadow evita waveform, nao e o dedup da mailbox.
}

void allocationFailure(unsigned failAt) {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  ListeningFeedback feedback;
  CHECK(feedback.begin(epd, canvas, powerOn));
  observed.store(&feedback);
  hw.failAllocationAt = failAt;
  CHECK(feedback.request(State::Listening));
  waitFor([&] { return !feedback.busy(); });
  CHECK(feedback.failed() && !feedback.ready());
  CHECK(feedback.error() == EPaperDisplay::Error::Allocation);
  CHECK(hw.allocations == failAt && hw.allocated.empty() && hw.io == 0);
  CHECK(!feedback.request(State::Speaking));
  feedback.finish();
}

void destructorJoins() {
  EPaperDisplay epd(200, 200, pins);
  Canvas canvas(epd);
  auto feedback = std::unique_ptr<ListeningFeedback>(new ListeningFeedback);
  CHECK(feedback->begin(epd, canvas, powerOn));
  observed.store(feedback.get());
  hw.hold = Stage::Full;
  CHECK(feedback->request(State::Listening));
  waitStage(Stage::Full);
  std::atomic<bool> returned{false};
  const unsigned before = yields;
  std::thread destroyer([&] { feedback.reset(); returned = true; });
  waitFor([&] { return yields.load() > before + 100; });
  CHECK(!returned);
  releaseStage();
  destroyer.join();
  observed.store(nullptr);
  CHECK(returned && hw.partialInit == 1);
  waitFor([] { return tasks.back()->deleted.load(); });
}
} // namespace

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *, uint32_t stack,
                                 void *argument, UBaseType_t priority, TaskHandle_t *task,
                                 BaseType_t core) {
  CHECK(stack == 8192 && priority == 2 && core == 1);
  if (failTask) return 0;
  ++taskCreations;
  tasks.emplace_back(new FeedbackTask);
  auto *created = tasks.back().get();
  *task = created;
  created->thread = std::thread([=] {
    currentTask = created;
    try { entry(argument); CHECK(false); } catch (const TaskExit &) {}
    created->deleted.store(true);
  });
  return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t task) {
  ++notifications;
  std::lock_guard<std::mutex> lock(task->mutex);
  ++task->notifications;
  task->cv.notify_all();
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout) {
  CHECK(currentTask && clear == pdTRUE && timeout == portMAX_DELAY);
  std::unique_lock<std::mutex> lock(currentTask->mutex);
  currentTask->cv.wait(lock, [] { return currentTask->notifications && !pauseDispatch; });
  const uint32_t count = currentTask->notifications;
  currentTask->notifications = 0;
  return count;
}

void vTaskDelay(TickType_t count) {
  CHECK(count > 0);
  ticks.fetch_add(count * 10);
  ++yields;
  std::this_thread::yield();
}

void vTaskDelete(TaskHandle_t task) {
  CHECK(task == nullptr && currentTask != nullptr); // Nenhuma delecao forcada.
  throw TaskExit();
}

uint32_t millis() { return ticks; }
uint32_t micros() { return ticks.load() * 1000; }
FeedbackSerial Serial;
void FeedbackSerial::println(const char *) { ++diagnostics; }
void FeedbackSerial::printf(const char *, ...) { ++diagnostics; }
size_t FeedbackSerial::write(const uint8_t *data, size_t size) {
  CHECK(size > 0 && size < 192 && data[size - 1] == '\n');
  ++diagnostics;
  return size;
}

void *heap_caps_malloc(size_t size, uint32_t) {
  assertOwnership();
  CHECK(size == 5000);
  if (++hw.allocations == hw.failAllocationAt) return nullptr;
  auto *memory = static_cast<uint8_t *>(std::malloc(size + 32));
  CHECK(memory);
  std::memset(memory, 0xA5, size + 32);
  hw.allocated.push_back(memory);
  return memory + 16;
}

void heap_caps_free(void *memory) {
  auto *allocation = static_cast<uint8_t *>(memory) - 16;
  for (int n = 0; n < 16; ++n) CHECK(allocation[n] == 0xA5 && allocation[5016 + n] == 0xA5);
  hw.allocated.erase(std::find(hw.allocated.begin(), hw.allocated.end(), allocation));
  std::free(allocation);
}

esp_err_t gpio_config(const gpio_config_t *) { assertOwnership(); ++hw.io; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  assertOwnership();
  ++hw.io;
  if (pin == pins.dc) hw.dc = level;
  return ESP_OK;
}
int gpio_get_level(gpio_num_t) { assertOwnership(); ++hw.io; return hw.busyStuck ? 1 : 0; }
esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t *, int) { assertOwnership(); ++hw.io; return ESP_OK; }
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *device) {
  assertOwnership();
  ++hw.io;
  *device = &hw;
  return ESP_OK;
}
esp_err_t spi_bus_free(spi_host_device_t) { return ESP_OK; }

esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t *transaction) {
  assertOwnership();
  ++hw.io;
  const auto *bytes = static_cast<const uint8_t *>(transaction->tx_buffer);
  const size_t size = transaction->length / 8;
  Stage stage = Stage::None;
  if (!hw.dc) {
    CHECK(size == 1);
    hw.command = bytes[0];
    if (hw.command == 0x12) { ++hw.init; stage = Stage::Init; }
    if (hw.command == 0x20) {
      if (hw.update == 0xC7) { ++hw.full; stage = Stage::Full; }
      else if (hw.update == 0xC0) { ++hw.partialInit; stage = Stage::PartialInit; }
      else if (hw.update == 0xCF) { ++hw.partial; stage = Stage::Partial; }
      if (stage == Stage::Full || stage == Stage::Partial) hw.frames.push_back(hw.ram24);
    }
  } else {
    if (hw.command == 0x22) hw.update = bytes[0];
    if (hw.command == 0x24) hw.ram24.assign(bytes, bytes + size);
    if (hw.command == 0x26) hw.ram26.assign(bytes, bytes + size);
  }
  if (stage != Stage::None) {
    std::unique_lock<std::mutex> lock(hw.mutex);
    if (hw.hold == stage && !hw.released) {
      hw.entered = stage;
      hw.cv.notify_all();
      hw.cv.wait(lock, [] { return hw.released; });
    }
    if (hw.fail == stage) return -1;
  }
  return ESP_OK;
}

int main() {
  renderer();
  resetHardware();
  auto listening = expectedFrame(State::Listening);
  resetHardware();
  auto speaking = expectedFrame(State::Speaking);
  resetHardware();
  beginAndCancel();
  resetHardware();
  firstFullAndCoalesce(listening, speaking);
  resetHardware();
  finishDuring(Stage::Full, listening);
  resetHardware();
  finishDuring(Stage::PartialInit, listening);
  resetHardware();
  for (Stage stage : {Stage::Init, Stage::Full, Stage::PartialInit, Stage::Partial}) {
    errorSticky(stage);
    resetHardware();
  }
  errorSticky(Stage::None, true);
  resetHardware();
  requestClaimStress();
  resetHardware();
  alreadyReadyAndPartialFinish(listening);
  resetHardware();
  allocationFailure(1);
  resetHardware();
  allocationFailure(2);
  resetHardware();
  destructorJoins();
  resetHardware();
  std::puts("ListeningFeedback real + renderer/Canvas/EPD real: 15 grupos OK");
}
