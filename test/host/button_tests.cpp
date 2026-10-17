#include "../../src/board/buttons.h"
#include <cassert>
#include <iostream>
#include <vector>
struct Event { BtnId id; BtnAction action; };
static std::vector<Event> events;
static void event(BtnId id, BtnAction action) { events.push_back({id, action}); }
static void advance(Buttons &buttons, uint32_t milliseconds) {
  for (uint32_t i = 0; i < milliseconds; i += 5) { Host::now += 5; buttons.poll(); }
}
int main() {
  Buttons buttons;
  buttons.begin(0, 18, event);
  ButtonHost::levels[0] = LOW; advance(buttons, 100);
  assert(events.empty());
  ButtonHost::levels[0] = HIGH; advance(buttons, 100);
  assert(events.size() == 1 && events[0].action == BtnAction::ShortClick);
  events.clear();
  advance(buttons, 400);
  ButtonHost::levels[0] = LOW; advance(buttons, 1500);
  assert(events.size() == 1 && events[0].action == BtnAction::LongPress);
  ButtonHost::levels[0] = HIGH; advance(buttons, 100);
  assert(events.size() == 2 && events[1].action == BtnAction::LongPressRelease);
  assert(events[0].id == BtnId::Boot && events[1].id == BtnId::Boot);
  events.clear(); advance(buttons, 400);
  ButtonHost::levels[18] = LOW; advance(buttons, 1500);
  ButtonHost::levels[18] = HIGH; advance(buttons, 100);
  assert(events.size() == 2 && events[0].id == BtnId::Pwr && events[1].id == BtnId::Pwr);
  assert(events[0].action == BtnAction::LongPress && events[1].action == BtnAction::LongPressRelease);
  events.clear(); advance(buttons, 400);
  ButtonHost::levels[0] = LOW; advance(buttons, 80);
  ButtonHost::levels[0] = HIGH; advance(buttons, 100);
  assert(events.size() == 1 && events[0].action == BtnAction::ShortClick);
  std::cout << "Buttons: 4/4 sequencias reais multi_button OK (clique/segurar/soltar/reuso)\n";
}
