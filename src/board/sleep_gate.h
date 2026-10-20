#pragma once
#include <cstdint>

// Level-triggered wake must not be armed with a button still held. A stuck
// button leaves the app in its quiescent state, without a reboot loop.
class SleepGate {
public:
  bool ready(uint32_t now, bool released, bool idle) {
    if (!released || !idle) { timing_ = false; return false; }
    if (!timing_) { since_ = now; timing_ = true; }
    return static_cast<uint32_t>(now - since_) >= 30;
  }
  void reset() { timing_ = false; }
private:
  uint32_t since_ = 0;
  bool timing_ = false;
};
