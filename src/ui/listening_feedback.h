#pragma once
#include "canvas.h"
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Um unico owner de UI chama begin/request/finish e possui este objeto (App
// global). Enquanto busy(), nenhum outro codigo pode usar Canvas/EPD ou cortar
// o rail. Antes de QUALQUER tela legada, chamar finish(), nao apenas busy().
class ListeningFeedback {
public:
  enum class State : uint8_t { Listening, Thinking, Speaking };
  using PowerOnFn = void (*)();

  ListeningFeedback() = default;
  ~ListeningFeedback();
  ListeningFeedback(const ListeningFeedback &) = delete;
  ListeningFeedback &operator=(const ListeningFeedback &) = delete;

  // Nao acessa hardware. Aloca a task uma unica vez; requests nao alocam.
  // powerOn deve SOMENTE ligar o rail EPD e esperar seus 10ms, sem Wire/sensor.
  bool begin(EPaperDisplay &epd, Canvas &canvas, PowerOnFn powerOn);
  bool request(State state);
  bool busy() const;

  // Cancela a mailbox e espera o ultimo acesso fisico com yield. Nao aborta
  // waveform nem retorna por timeout enquanto o SDK ainda usa Canvas/EPD.
  // Pode voltar a receber request() apos retornar.
  void finish();

  // Snapshots atomicos do worker: ready so apos refresh + initPartial completos.
  // Falhas sao sticky; nao consultar o driver diretamente enquanto busy().
  bool ready() const { return ready_.load(std::memory_order_acquire); }
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  EPaperDisplay::Error error() const { return error_.load(std::memory_order_acquire); }

private:
  static constexpr uint32_t kStateMask = 3;
  static constexpr uint32_t kPending = 4;
  static constexpr uint32_t kActive = 8;
  static constexpr uint32_t kValid = 16;
  static constexpr uint32_t kTicket = 32;
  static constexpr uint32_t kTicketMask = ~(kTicket - 1);

  EPaperDisplay *epd_ = nullptr;
  Canvas *canvas_ = nullptr;
  PowerOnFn powerOn_ = nullptr;
  TaskHandle_t task_ = nullptr;
  // Pending->active e a publicacao de outro pedido sao CAS da MESMA palavra:
  // nao existe janela busy=false entre mailbox consumida e worker dono.
  std::atomic<uint32_t> mailbox_{0};
  std::atomic<uint32_t> requestUs_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> exited_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> failed_{false};
  std::atomic<EPaperDisplay::Error> error_{EPaperDisplay::Error::None};

  static void taskEntry(void *argument);
  void run();
  void render(State state);
};
