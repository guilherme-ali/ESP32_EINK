#include "../../src/board/sleep_gate.h"
#include "../../src/board/wake_intent.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

int main() {
  SleepGate gate;
  assert(!gate.ready(0, true, true));
  assert(!gate.ready(29, true, true));
  assert(gate.ready(30, true, true));
  assert(!gate.ready(40, false, true));
  assert(!gate.ready(10000, false, true)); // stuck key never arms LOW wake
  assert(!gate.ready(10001, true, true));
  assert(!gate.ready(10030, true, true));
  assert(gate.ready(10031, true, true));
  assert(!gate.ready(10032, true, false)); // Saving/worker must veto sleep
  assert(!gate.ready(11000, true, true));
  assert(gate.ready(11030, true, true));
  gate.reset();
  assert(!gate.ready(UINT32_MAX - 10, true, true));
  assert(!gate.ready(18, true, true));
  assert(gate.ready(19, true, true));
  for (int boot = 0; boot < 2; ++boot)
    for (int pwr = 0; pwr < 2; ++pwr)
      for (int low = 0; low < 2; ++low)
        assert((wakeIntent(boot, pwr, low) == WakeIntent::Capture) == (boot && !pwr && !low));
  std::puts("Sleep gate / wake intent: stable release, active-job veto, stuck key, wrap, PWR priority OK");
}
