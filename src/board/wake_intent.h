#pragma once

enum class WakeIntent { Home, Capture };
inline WakeIntent wakeIntent(bool bootWake, bool pwrWake, bool pwrLow) {
  return bootWake && !pwrWake && !pwrLow ? WakeIntent::Capture : WakeIntent::Home;
}
