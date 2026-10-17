#pragma once
#include "../stubs/Arduino.h"
#include <array>
constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT_PULLUP = 2;
namespace ButtonHost {
inline std::array<int, 256> levels = [] { std::array<int, 256> a{}; a.fill(HIGH); return a; }();
}
inline void pinMode(int, int) {}
inline int digitalRead(uint8_t pin) { return ButtonHost::levels[pin]; }
