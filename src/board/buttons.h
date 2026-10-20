#pragma once
#include <atomic>
#include <stdint.h>

enum class BtnId : uint8_t { Boot = 1, Pwr = 2 };

enum class BtnAction : uint8_t {
  None,
  ShortClick,
  LongPress,
  LongPressRelease,
  PressDown,
};

using ButtonEventFn = void (*)(BtnId id, BtnAction action);

class Buttons {
public:
  // Inicializar/reinicializar somente com o produtor parado. Faz a primeira
  // observacao, sem callbacks; nao recupera historia anterior ao begin().
  void begin(uint8_t bootPin, uint8_t pwrPin, ButtonEventFn onEvent);
  // EXT1 proves a press occurred. Call only before starting the sampler task.
  // Already released: queue one short click. Still LOW: seed the observed press.
  void latchWake(BtnId id);
  // Unico produtor, normalmente task de 5ms. Nunca chama callbacks.
  void sample();
  // Mudar modo somente durante handoff, sem sample()/poll() concorrentes.
  void useExternalSampling(bool enabled);
  // Unico consumidor/UI: callback, troca de callback, descarte e supressao.
  // poll amostra uma vez no modo local; drena no maximo 32 eventos por chamada.
  void poll();
  void setCallback(ButtonEventFn onEvent);
  bool eventsLost() const;
  // Descarta o snapshot da fila e limpa perda observada. Overflow concorrente
  // pode manter eventsLost true. Nao reinicia gestos: suprimir se necessario.
  void discardEvents();
  // Invalida tambem eventos ja enfileirados deste botao. Se ja estava solto
  // na ultima observacao, nao consome o proximo gesto. Seguro com sample().
  void suppressUntilRelease(BtnId id);
  bool pressed(BtnId id) const; // estado estavel/debounced, leitura thread-safe
  uint32_t maxSampleGapMs() const { return maxGapMs_.load(); }
  uint32_t sampleGapCount() const { return gapCount_.load(); }
  uint32_t lastEventObservedMs() const { return lastEventMs_; }

private:
  struct State {
    uint8_t pin = 0;
    bool candidate = false;
    bool down = false;
    bool longFired = false;
    bool suppressed = false;
    uint32_t candidateSince = 0;
    uint32_t downSince = 0;
    uint32_t command = 0;
  };
  struct Event {
    BtnId id;
    BtnAction action;
    uint32_t command;
    uint32_t observedMs;
  };
  static constexpr uint32_t kCapacity = 32;
  void enqueue(BtnId id, BtnAction action, uint32_t command);
  State states_[2]{}; // somente produtor
  Event queue_[kCapacity]{};
  std::atomic<uint32_t> head_{0}; // somente produtor escreve
  std::atomic<uint32_t> tail_{0}; // somente consumidor escreve
  std::atomic<bool> lost_{false};
  std::atomic<bool> external_{false};
  std::atomic<bool> pressed_[2]{};
  std::atomic<bool> observedActive_[2]{};
  // Geracao nos bits altos; bit0 pede supressao do gesto observado.
  std::atomic<uint32_t> commands_[2]{};
  ButtonEventFn onEvent_ = nullptr; // somente consumidor
  uint32_t lastSampleMs_ = 0;
  std::atomic<uint32_t> maxGapMs_{0}, gapCount_{0};
  uint32_t lastEventMs_ = 0; // consumer only
  bool initialized_ = false;
  bool sampled_ = false;
};
