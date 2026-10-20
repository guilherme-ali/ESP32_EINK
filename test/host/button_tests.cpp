#include <Arduino.h>
#include "../../src/board/buttons.h"
#include "../../src/board/button_chord.h"
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <thread>
#include <vector>

struct Event { BtnId id; BtnAction action; };
static std::vector<Event> events;
static std::thread::id uiThread;
static int alternateCalls = 0;

static void event(BtnId id, BtnAction action) {
  assert(std::this_thread::get_id() == uiThread);
  events.push_back({id, action});
}
static void alternate(BtnId id, BtnAction action) {
  ++alternateCalls;
  event(id, action);
}
static void reset() {
  ButtonHost::reset();
  events.clear();
  alternateCalls = 0;
}
static void advance(Buttons &buttons, uint32_t ms, bool external = false) {
  for (uint32_t i = 0; i < ms; i += 5) {
    Host::now += 5;
    if (external) buttons.sample(); else buttons.poll();
  }
}
static void level(Buttons &buttons, uint8_t pin, int value, bool external = false) {
  ButtonHost::levels[pin] = value;
  if (external) buttons.sample(); else buttons.poll();
}
static void click(Buttons &buttons, uint8_t pin = 0, bool external = false) {
  level(buttons, pin, LOW, external);
  advance(buttons, 20, external);
  level(buttons, pin, HIGH, external);
  advance(buttons, 20, external);
}
static void expect(BtnId id, std::initializer_list<BtnAction> actions) {
  assert(events.size() == actions.size());
  size_t at = 0;
  for (BtnAction action : actions) {
    assert(events[at].id == id && events[at].action == action);
    ++at;
  }
}

static void debounceAndShort() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  assert(ButtonHost::modes[0] == INPUT_PULLUP && ButtonHost::modes[18] == INPUT_PULLUP);
  assert(ButtonHost::reads == 2 && events.empty());
  level(buttons, 0, LOW);
  advance(buttons, 10);
  assert(events.empty() && !buttons.pressed(BtnId::Boot));
  advance(buttons, 5);
  expect(BtnId::Boot, {BtnAction::PressDown});
  assert(buttons.pressed(BtnId::Boot));
  level(buttons, 0, HIGH);
  advance(buttons, 10);
  expect(BtnId::Boot, {BtnAction::PressDown});
  advance(buttons, 5);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
  assert(!buttons.pressed(BtnId::Boot) && !buttons.eventsLost());
  // Sem janela de duplo clique: um segundo gesto imediato e independente.
  click(buttons);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick,
                       BtnAction::PressDown, BtnAction::ShortClick});
}

static void bounce() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  for (int i = 0; i < 10; ++i) {
    level(buttons, 0, LOW); advance(buttons, 5);
    level(buttons, 0, HIGH); advance(buttons, 5);
  }
  assert(events.empty());
  level(buttons, 0, LOW); advance(buttons, 15);
  for (int i = 0; i < 10; ++i) {
    level(buttons, 0, HIGH); advance(buttons, 5);
    level(buttons, 0, LOW); advance(buttons, 5);
  }
  expect(BtnId::Boot, {BtnAction::PressDown});
  level(buttons, 0, HIGH); advance(buttons, 15);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
}

static void longAndReuse() {
  for (uint8_t pin : {uint8_t{0}, uint8_t{18}}) {
    reset();
    Buttons buttons;
    buttons.begin(0, 18, event);
    const BtnId id = pin == 0 ? BtnId::Boot : BtnId::Pwr;
    level(buttons, pin, LOW);
    advance(buttons, 995);
    expect(id, {BtnAction::PressDown});
    advance(buttons, 5);
    expect(id, {BtnAction::PressDown, BtnAction::LongPress});
    advance(buttons, 2000);
    expect(id, {BtnAction::PressDown, BtnAction::LongPress});
    level(buttons, pin, HIGH); advance(buttons, 15);
    expect(id, {BtnAction::PressDown, BtnAction::LongPress, BtnAction::LongPressRelease});
    events.clear();
    click(buttons, pin);
    expect(id, {BtnAction::PressDown, BtnAction::ShortClick});
  }
  // Soltar antes do limiar nao vira longo durante o debounce da soltura.
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  level(buttons, 0, LOW); advance(buttons, 995);
  level(buttons, 0, HIGH); advance(buttons, 15);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
}

static void gapsAndNoCatchup() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  ButtonHost::levels[0] = LOW;
  Host::now += 5000;
  const uint32_t before = ButtonHost::reads.load();
  buttons.poll();
  assert(ButtonHost::reads == before + 2 && events.empty());
  for (int i = 0; i < 100; ++i) buttons.poll();
  assert(events.empty()); // repeticao do nivel no mesmo instante nao e tempo
  advance(buttons, 15);
  expect(BtnId::Boot, {BtnAction::PressDown});
  advance(buttons, 900);
  Host::now += 5000;
  buttons.poll();
  assert(buttons.pressed(BtnId::Boot)); // preservar estavel, nao inferir historia
  advance(buttons, 995);
  expect(BtnId::Boot, {BtnAction::PressDown});
  advance(buttons, 5);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress});
  Host::now += 5000; buttons.poll(); advance(buttons, 1000);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress});
  level(buttons, 0, HIGH); advance(buttons, 15);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress, BtnAction::LongPressRelease});

  reset(); buttons.begin(0, 18, event);
  level(buttons, 0, LOW); advance(buttons, 10);
  Host::now += 51; buttons.poll();
  advance(buttons, 10); assert(events.empty());
  advance(buttons, 5); expect(BtnId::Boot, {BtnAction::PressDown});
  // Polls espacos de 51ms nunca provam longa; soltura ainda vira curto.
  for (int i = 0; i < 100; ++i) { Host::now += 51; buttons.poll(); }
  assert(buttons.maxSampleGapMs() == 51 && buttons.sampleGapCount() == 101);
  expect(BtnId::Boot, {BtnAction::PressDown});
  level(buttons, 0, HIGH); advance(buttons, 15);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});

  reset(); buttons.begin(0, 18, event);
  assert(buttons.maxSampleGapMs() == 0 && buttons.sampleGapCount() == 0);
  level(buttons, 0, LOW); advance(buttons, 950);
  Host::now += 50; buttons.poll(); // limite de 50ms ainda e amostragem regular
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress});
}

static void externalAndCallback() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  buttons.useExternalSampling(true);
  level(buttons, 0, LOW, true); advance(buttons, 1200, true);
  level(buttons, 0, HIGH, true); advance(buttons, 20, true);
  assert(events.empty() && !buttons.pressed(BtnId::Boot));
  buttons.setCallback(alternate);
  const uint32_t before = ButtonHost::reads.load();
  Host::now += 10000;
  buttons.poll();
  assert(ButtonHost::reads == before && alternateCalls == 3);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress, BtnAction::LongPressRelease});
  events.clear();
  buttons.setCallback(nullptr);
  click(buttons, 0, true); buttons.poll();
  assert(events.empty());
  buttons.setCallback(event);
  buttons.useExternalSampling(false); // produtor externo ja parado
  click(buttons);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
}

static void overflowAndDiscard() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  buttons.useExternalSampling(true);
  for (int i = 0; i < 16; ++i) click(buttons, 0, true);
  assert(!buttons.eventsLost()); // capacidade util inteira: 32, nao 31
  click(buttons, 18, true);
  assert(buttons.eventsLost() && events.empty());
  buttons.poll();
  assert(events.size() == 32 && buttons.eventsLost());
  for (size_t i = 0; i < events.size(); ++i) {
    assert(events[i].id == BtnId::Boot);
    assert(events[i].action == (i % 2 == 0 ? BtnAction::PressDown : BtnAction::ShortClick));
  }
  buttons.discardEvents();
  assert(!buttons.eventsLost());
  events.clear();
  click(buttons, 18, true); buttons.discardEvents(); buttons.poll();
  assert(events.empty());
  click(buttons, 18, true); buttons.poll();
  expect(BtnId::Pwr, {BtnAction::PressDown, BtnAction::ShortClick});
}

static void suppression() {
  // Wake mantido desde begin: suprimir inclusive candidato ainda sem PressDown.
  for (uint8_t pin : {uint8_t{0}, uint8_t{18}}) {
    reset(); ButtonHost::levels[pin] = LOW;
    Buttons buttons;
    buttons.begin(0, 18, event);
    buttons.useExternalSampling(true);
    const BtnId id = pin == 0 ? BtnId::Boot : BtnId::Pwr;
    buttons.suppressUntilRelease(id);
    advance(buttons, 2000, true);
    assert(buttons.pressed(id));
    level(buttons, pin, HIGH, true); advance(buttons, 10, true);
    level(buttons, pin, LOW, true); advance(buttons, 10, true); // release bounce
    level(buttons, pin, HIGH, true); advance(buttons, 15, true);
    buttons.poll(); assert(events.empty());
    click(buttons, pin, true); buttons.poll();
    expect(id, {BtnAction::PressDown, BtnAction::ShortClick});
  }
  // Invalidar PressDown + longa + soltura ja na fila e nao comer proximo gesto.
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event); buttons.useExternalSampling(true);
  level(buttons, 0, LOW, true); advance(buttons, 1200, true);
  level(buttons, 0, HIGH, true); advance(buttons, 15, true);
  buttons.suppressUntilRelease(BtnId::Boot);
  click(buttons, 0, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});

  // BOOT curta sintetizada pelo main pois ja solta: supressao idle nao arma
  // o proximo clique, mesmo sem sample intermediaria entre pedido e novo LOW.
  reset(); buttons.begin(0, 18, event); buttons.useExternalSampling(true);
  buttons.suppressUntilRelease(BtnId::Boot);
  click(buttons, 0, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});

  // Supressao de PWR nao afeta BOOT; callback de longa ja entregue nao repete
  // nem recebe release quando gesto foi consumido pela UI.
  reset(); buttons.begin(0, 18, event); buttons.useExternalSampling(true);
  level(buttons, 18, LOW, true); advance(buttons, 1200, true); buttons.poll();
  expect(BtnId::Pwr, {BtnAction::PressDown, BtnAction::LongPress});
  events.clear(); buttons.suppressUntilRelease(BtnId::Pwr);
  click(buttons, 0, true);
  level(buttons, 18, HIGH, true); advance(buttons, 15, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
}

static void wrapAndIndependentInstances() {
  reset(); Host::resetClock(UINT32_MAX - 10);
  Buttons first, second;
  first.begin(0, 18, event); second.begin(1, 19, alternate);
  level(first, 0, LOW); advance(first, 1000);
  level(first, 0, HIGH); advance(first, 15);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress, BtnAction::LongPressRelease});
  assert(alternateCalls == 0);
  events.clear();
  for (int i = 0; i < 300; ++i) click(first); // varias voltas no ring
  assert(events.size() == 600 && !first.eventsLost());
  events.clear();
  click(second, 1); // apos lacuna, primeira observacao nao fabrica gesto
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
  assert(alternateCalls == 2);
  first.begin(0, 18, event); // rebegin seguro sem lista global multi_button
  events.clear(); click(first);
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});
}

static void simultaneous() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  ButtonHost::levels[0] = ButtonHost::levels[18] = LOW;
  buttons.poll(); advance(buttons, 1000);
  ButtonHost::levels[0] = ButtonHost::levels[18] = HIGH;
  buttons.poll(); advance(buttons, 15);
  assert(events.size() == 6);
  for (size_t i = 0; i < events.size(); i += 2) {
    assert(events[i].id == BtnId::Boot && events[i + 1].id == BtnId::Pwr);
    const BtnAction action = i == 0 ? BtnAction::PressDown :
                             (i == 2 ? BtnAction::LongPress : BtnAction::LongPressRelease);
    assert(events[i].action == action && events[i + 1].action == action);
  }
}

static void concurrentSpsc() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event); buttons.useExternalSampling(true);
  std::atomic<size_t> consumed{0};
  std::atomic<bool> done{false};
  std::thread producer([&] {
    for (size_t batch = 0; batch < 200; ++batch) {
      for (int j = 0; j < 8; ++j) click(buttons, j % 2 ? 18 : 0, true);
      // Cada lote cabe inteiro; consumidor pode drenar em paralelo.
      while (consumed.load(std::memory_order_acquire) < (batch + 1) * 16) {
        std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });
  while (!done.load(std::memory_order_acquire)) {
    buttons.poll();
    consumed.store(events.size(), std::memory_order_release);
    std::this_thread::yield();
  }
  producer.join(); buttons.poll();
  assert(events.size() == 3200 && !buttons.eventsLost());
  for (size_t i = 0; i < events.size(); ++i) {
    assert(events[i].id == ((i / 2) % 2 ? BtnId::Pwr : BtnId::Boot));
    assert(events[i].action == (i % 2 ? BtnAction::ShortClick : BtnAction::PressDown));
  }
}

static void concurrentSuppression() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event); buttons.useExternalSampling(true);
  std::atomic<int> phase{0};
  std::thread producer([&] {
    for (int i = 0; i < 200; ++i) {
      level(buttons, 0, LOW, true); advance(buttons, 20, true);
      phase.store(i * 4 + 1, std::memory_order_release);
      while (phase.load(std::memory_order_acquire) != i * 4 + 2) std::this_thread::yield();
      level(buttons, 0, HIGH, true); advance(buttons, 20, true);
      phase.store(i * 4 + 3, std::memory_order_release);
      while (phase.load(std::memory_order_acquire) != i * 4 + 4) std::this_thread::yield();
    }
  });
  for (int i = 0; i < 200; ++i) {
    while (phase.load(std::memory_order_acquire) != i * 4 + 1) std::this_thread::yield();
    assert(buttons.pressed(BtnId::Boot));
    if (i % 2 == 0) buttons.suppressUntilRelease(BtnId::Boot);
    phase.store(i * 4 + 2, std::memory_order_release);
    while (phase.load(std::memory_order_acquire) != i * 4 + 3) std::this_thread::yield();
    buttons.poll();
    assert(events.size() == static_cast<size_t>((i + 1) / 2) * 2);
    phase.store(i * 4 + 4, std::memory_order_release);
  }
  producer.join();
  assert(!buttons.eventsLost());
}

static void wakeLatch() {
  reset();
  Buttons buttons;
  buttons.begin(0, 18, event);
  buttons.latchWake(BtnId::Boot); // pulse already over before first observation
  buttons.useExternalSampling(true);
  buttons.poll();
  expect(BtnId::Boot, {BtnAction::ShortClick});
  events.clear(); click(buttons, 0, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});

  reset(); ButtonHost::levels[0] = LOW;
  buttons.begin(0, 18, event); buttons.latchWake(BtnId::Boot);
  buttons.useExternalSampling(true);
  level(buttons, 0, HIGH, true); advance(buttons, 15, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::ShortClick});

  reset(); ButtonHost::levels[0] = LOW;
  buttons.begin(0, 18, event); buttons.latchWake(BtnId::Boot);
  buttons.useExternalSampling(true);
  advance(buttons, 1000, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress});
  level(buttons, 0, HIGH, true); advance(buttons, 15, true); buttons.poll();
  expect(BtnId::Boot, {BtnAction::PressDown, BtnAction::LongPress, BtnAction::LongPressRelease});
}

static void priorityChord() {
  for (bool bootFirst : {true, false}) {
    ButtonChord chord;
    BtnId first = bootFirst ? BtnId::Boot : BtnId::Pwr;
    BtnId second = bootFirst ? BtnId::Pwr : BtnId::Boot;
    assert(!chord.consume(first, BtnAction::PressDown));
    assert(chord.consume(second, BtnAction::PressDown));
    assert(chord.consume(BtnId::Boot, BtnAction::LongPress));
    assert(chord.consume(first, BtnAction::ShortClick));
    assert(chord.consume(second, BtnAction::ShortClick));
    assert(!chord.consume(BtnId::Boot, BtnAction::PressDown));
    assert(!chord.consume(BtnId::Boot, BtnAction::ShortClick));
  }
}

int main() {
  uiThread = std::this_thread::get_id();
  debounceAndShort();
  bounce();
  longAndReuse();
  gapsAndNoCatchup();
  externalAndCallback();
  overflowAndDiscard();
  suppression();
  wrapAndIndependentInstances();
  simultaneous();
  concurrentSpsc();
  concurrentSuppression();
  wakeLatch();
  priorityChord();
  std::cout << "Buttons: 13/13 grupos OK (tempo real, SPSC, wake latch, chord, overflow, suppress, wrap)\n";
}
