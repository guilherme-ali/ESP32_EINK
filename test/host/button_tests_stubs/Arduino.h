#pragma once
#include "../stubs/Arduino.h"
#include <array>
constexpr int INPUT_PULLUP = 2;
namespace ButtonHost {
inline std::array<int, 256> levels = [] { std::array<int, 256> a{}; a.fill(HIGH); return a; }();
}
inline int digitalRead(uint8_t pin) { return ButtonHost::levels[pin]; }
