#pragma once
#include <array>
#include <atomic>
#include <cstdint>

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT_PULLUP = 2;
namespace Host {
inline std::atomic<uint32_t> now{0};
inline void resetClock(uint32_t value = 0) { now.store(value); }
}
namespace ButtonHost {
inline std::array<std::atomic<int>, 256> levels{};
inline std::array<int, 256> modes{};
inline std::atomic<uint32_t> reads{0};
inline void reset() {
  Host::resetClock();
  for (auto &level : levels) level.store(HIGH);
  modes.fill(-1);
  reads.store(0);
}
}
inline void pinMode(uint8_t pin, int mode) { ButtonHost::modes[pin] = mode; }
inline int digitalRead(uint8_t pin) {
  ButtonHost::reads.fetch_add(1);
  return ButtonHost::levels[pin].load();
}
inline uint32_t millis() { return Host::now.load(); }
