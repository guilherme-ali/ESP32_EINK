#pragma once
#include <cstdint>
#include <deque>
namespace Host { inline std::deque<uint32_t> randomValues; }
inline uint32_t esp_random() {
  if (Host::randomValues.empty()) return 0x12345678U;
  uint32_t value = Host::randomValues.front(); Host::randomValues.pop_front(); return value;
}
