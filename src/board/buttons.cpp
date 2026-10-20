#include "buttons.h"
#include <Arduino.h>

namespace {
constexpr uint32_t kDebounceMs = 15;
constexpr uint32_t kLongMs = 1000;
constexpr uint32_t kMaxSampleGapMs = 50;

int indexOf(BtnId id) {
  return id == BtnId::Boot ? 0 : (id == BtnId::Pwr ? 1 : -1);
}
} // namespace

void Buttons::begin(uint8_t bootPin, uint8_t pwrPin, ButtonEventFn onEvent) {
  initialized_ = false;
  sampled_ = false;
  maxGapMs_.store(0);
  gapCount_.store(0);
  lastEventMs_ = 0;
  onEvent_ = onEvent;
  external_.store(false, std::memory_order_relaxed);
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  lost_.store(false, std::memory_order_relaxed);
  for (int i = 0; i < 2; ++i) {
    states_[i] = State{};
    states_[i].pin = i == 0 ? bootPin : pwrPin;
    pressed_[i].store(false, std::memory_order_relaxed);
    observedActive_[i].store(false, std::memory_order_relaxed);
    commands_[i].store(0, std::memory_order_relaxed);
    pinMode(states_[i].pin, INPUT_PULLUP);
  }
  initialized_ = true;
  sample();
}

void Buttons::setCallback(ButtonEventFn onEvent) {
  onEvent_ = onEvent;
}

void Buttons::latchWake(BtnId id) {
  const int i = indexOf(id);
  if (!initialized_ || external_.load() || i < 0) return;
  State &s = states_[i];
  const uint32_t now = millis();
  const bool low = digitalRead(s.pin) == LOW;
  s.candidate = s.down = low;
  s.candidateSince = s.downSince = now;
  s.longFired = s.suppressed = false;
  s.command = commands_[i].load(std::memory_order_relaxed);
  pressed_[i].store(low, std::memory_order_relaxed);
  observedActive_[i].store(low, std::memory_order_relaxed);
  enqueue(id, low ? BtnAction::PressDown : BtnAction::ShortClick, s.command);
}

void Buttons::useExternalSampling(bool enabled) {
  external_.store(enabled, std::memory_order_relaxed);
}

bool Buttons::eventsLost() const {
  return lost_.load(std::memory_order_acquire);
}

void Buttons::discardEvents() {
  // Limpar antes do snapshot evita apagar uma nova falha concorrente.
  lost_.exchange(false, std::memory_order_acq_rel);
  tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
}

bool Buttons::pressed(BtnId id) const {
  const int i = indexOf(id);
  return i >= 0 && pressed_[i].load(std::memory_order_acquire);
}

void Buttons::suppressUntilRelease(BtnId id) {
  const int i = indexOf(id);
  if (i < 0) return;
  const uint32_t old = commands_[i].load(std::memory_order_relaxed);
  const bool active = observedActive_[i].load(std::memory_order_acquire);
  commands_[i].store(((old & ~uint32_t{1}) + 2) | (active ? 1 : 0),
                     std::memory_order_release);
}

void Buttons::enqueue(BtnId id, BtnAction action, uint32_t command) {
  const uint32_t head = head_.load(std::memory_order_relaxed);
  if (head - tail_.load(std::memory_order_acquire) == kCapacity) {
    lost_.store(true, std::memory_order_release);
    return; // preservar prefixo, nunca sobrescrever evento nao consumido
  }
  queue_[head % kCapacity] = Event{id, action, command, millis()};
  head_.store(head + 1, std::memory_order_release);
}

void Buttons::sample() {
  if (!initialized_) return;
  const uint32_t now = millis();
  const uint32_t elapsed = sampled_ ? uint32_t(now - lastSampleMs_) : 0;
  if (elapsed > maxGapMs_.load()) maxGapMs_.store(elapsed);
  const bool gap = elapsed > kMaxSampleGapMs;
  if (gap) gapCount_.fetch_add(1);
  for (int i = 0; i < 2; ++i) {
    State &s = states_[i];
    const BtnId id = i == 0 ? BtnId::Boot : BtnId::Pwr;
    const bool raw = digitalRead(s.pin) == LOW;
    const uint32_t command = commands_[i].load(std::memory_order_acquire);
    if (command != s.command) {
      // s.down cobre PressDown publicado simultaneamente ao pedido da UI.
      s.suppressed = (command & 1) || s.down;
      s.command = command;
    }
    if (!sampled_ || gap || raw != s.candidate) {
      s.candidate = raw;
      s.candidateSince = now;
      if (raw) s.downSince = now;
    }
    if (raw != s.down && uint32_t(now - s.candidateSince) >= kDebounceMs) {
      s.down = raw;
      pressed_[i].store(raw, std::memory_order_release);
      if (raw) {
        s.longFired = false;
        if (!s.suppressed) enqueue(id, BtnAction::PressDown, command);
      } else {
        if (!s.suppressed) {
          enqueue(id, s.longFired ? BtnAction::LongPressRelease : BtnAction::ShortClick,
                  command);
        }
        s.longFired = false;
      }
    }
    if (s.down && raw && !s.longFired && !s.suppressed &&
        uint32_t(now - s.downSince) >= kLongMs) {
      s.longFired = true;
      enqueue(id, BtnAction::LongPress, command);
    }
    if (!s.down && !raw && uint32_t(now - s.candidateSince) >= kDebounceMs) {
      s.suppressed = false;
    }
    observedActive_[i].store(s.down || raw, std::memory_order_release);
  }
  sampled_ = true;
  lastSampleMs_ = now;
}

void Buttons::poll() {
  if (!external_.load(std::memory_order_relaxed)) sample();
  for (uint32_t n = 0; n < kCapacity; ++n) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) break;
    const Event event = queue_[tail % kCapacity];
    tail_.store(tail + 1, std::memory_order_release);
    const int i = indexOf(event.id);
    if (event.command == commands_[i].load(std::memory_order_acquire) && onEvent_) {
      lastEventMs_ = event.observedMs;
      onEvent_(event.id, event.action);
    }
  }
}
