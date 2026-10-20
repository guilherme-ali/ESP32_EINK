#include "listening_feedback.h"
#include "screens.h"
#include "../board/boot_trace.h"

ListeningFeedback::~ListeningFeedback() {
  finish();
  if (!task_) return;
  stopping_.store(true, std::memory_order_release);
  xTaskNotifyGive(task_);
  while (!exited_.load(std::memory_order_acquire)) vTaskDelay(1);
}

bool ListeningFeedback::begin(EPaperDisplay &epd, Canvas &canvas, PowerOnFn powerOn) {
  if (task_) return epd_ == &epd && canvas_ == &canvas && powerOn_ == powerOn;
  if (!powerOn) return false;
  epd_ = &epd;
  canvas_ = &canvas;
  powerOn_ = powerOn;
  // ESP-IDF mede stack em bytes. A task permanece bloqueada quando ociosa.
  if (xTaskCreatePinnedToCore(taskEntry, "voice_display", 8192, this, 2, &task_, 1) == pdPASS)
    return true;
  task_ = nullptr;
  epd_ = nullptr;
  canvas_ = nullptr;
  powerOn_ = nullptr;
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.println("[ListeningFeedback] task creation failed");
#endif
  return false;
}

bool ListeningFeedback::request(State state) {
  if (!task_ || failed() || static_cast<uint32_t>(state) > static_cast<uint32_t>(State::Speaking))
    return false;
  uint32_t word = mailbox_.load(std::memory_order_acquire);
  uint32_t next;
  do {
    // App pode repetir o estado a cada loop. Nao invalidar o claim nem
    // notificar/desenhar de novo enquanto o mesmo estado ja estiver vigente.
    if ((word & kValid) && (word & kStateMask) == static_cast<uint32_t>(state)) return true;
#if ARDUINO_USB_CDC_ON_BOOT
    requestUs_.store(micros(), std::memory_order_release);
#endif
    next = ((word + kTicket) & kTicketMask) | (word & kActive) | kPending | kValid |
           static_cast<uint32_t>(state);
  } while (!mailbox_.compare_exchange_weak(word, next, std::memory_order_acq_rel));
  xTaskNotifyGive(task_);
  return true;
}

bool ListeningFeedback::busy() const {
  return (mailbox_.load(std::memory_order_acquire) & (kPending | kActive)) != 0;
}

void ListeningFeedback::finish() {
  uint32_t word = mailbox_.load(std::memory_order_acquire);
  uint32_t next;
  do {
    next = ((word + kTicket) & kTicketMask) | (word & kActive);
  } while (!mailbox_.compare_exchange_weak(word, next, std::memory_order_acq_rel));
  while (busy()) vTaskDelay(1);
}

void ListeningFeedback::taskEntry(void *argument) {
  auto *self = static_cast<ListeningFeedback *>(argument);
  self->run();
  // Ultimo acesso ao objeto. TCB/stack dinamicos pertencem ao RTOS, nao ao App;
  // a destruicao do App nao pode liberar memoria ainda usada por vTaskDelete.
  self->exited_.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void ListeningFeedback::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (stopping_.load(std::memory_order_acquire)) return;
    uint32_t word = mailbox_.load(std::memory_order_acquire);
    while (word & kPending) {
      const uint32_t claimed = (word & ~kPending) | kActive;
      if (!mailbox_.compare_exchange_weak(word, claimed, std::memory_order_acq_rel)) continue;
      // finish (ou um pedido mais recente) pode invalidar um claim antes do IO.
      if (!failed() && (mailbox_.load(std::memory_order_acquire) & kTicketMask) ==
                           (claimed & kTicketMask))
        render(static_cast<State>(claimed & kStateMask));
      // Release somente APOS shadow, LUT parcial, snapshots e diagnosticos.
      // Preserva um request publicado durante o refresh: busy segue verdadeiro.
      mailbox_.fetch_and(~kActive, std::memory_order_release);
      word = mailbox_.load(std::memory_order_acquire);
    }
  }
}

void ListeningFeedback::render(State state) {
#if ARDUINO_USB_CDC_ON_BOOT
  const uint32_t requestUs = requestUs_.load(std::memory_order_acquire);
  const uint32_t refreshBeginUs = micros();
#endif
  powerOn_();
  bool ok = epd_->healthy();
  const bool needsBase = !epd_->ready();
  if (ok && needsBase) ok = epd_->init();
  if (ok) {
    const char *title = "Ouvindo";
    const char *hint = "Pode falar";
    if (state == State::Thinking) {
      title = "Pensando";
      hint = "Aguarde um instante";
    } else if (state == State::Speaking) {
      title = "Falando";
      hint = "PWR cancela";
    }
    Screens::paintVoiceState(*canvas_, title, hint, state == State::Listening);
    // Nunca mostrar FULL branco antes do conteudo, nem parcial sem base apos
    // boot/rail perdido. Um driver ja pronto pressupoe base/rail mantidos.
    ok = needsBase ? (epd_->displayPartBaseImage() && epd_->initPartial()) : epd_->displayPart();
  }
  error_.store(epd_->error(), std::memory_order_release);
  ready_.store(ok && epd_->ready(), std::memory_order_release);
  if (!ok) {
    failed_.store(true, std::memory_order_release);
  } else {
    BootTrace::mark(BootTrace::DisplayReady);
  }
#if ARDUINO_USB_CDC_ON_BOOT
  const uint32_t refreshDoneUs = micros();
#if defined(ARDUINO_ARCH_ESP32)
  const unsigned stackFree = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
#else
  const unsigned stackFree = 0;
#endif
  // Print::printf aloca para linhas >64B; buffer fixo tambem no diagnostico.
  char line[192];
  const int length = snprintf(line, sizeof(line),
      "[ListeningFeedback] state=%u request_us=%lu refresh_begin_us=%lu refresh_done_us=%lu ok=%u error=%u stack_min_bytes=%u\n",
      static_cast<unsigned>(state), static_cast<unsigned long>(requestUs),
      static_cast<unsigned long>(refreshBeginUs), static_cast<unsigned long>(refreshDoneUs),
      static_cast<unsigned>(ok), static_cast<unsigned>(epd_->error()), stackFree);
  if (length > 0 && length < static_cast<int>(sizeof(line)))
    Serial.write(reinterpret_cast<const uint8_t *>(line), static_cast<size_t>(length));
#endif
}
