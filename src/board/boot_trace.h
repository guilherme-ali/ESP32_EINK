#pragma once
#include <atomic>
#include <cstdint>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_timer.h>
#endif

// First occurrence only, in one boot's monotonic clock. DMA data is not proof
// of analog readiness; that requires the separate acoustic bench measurement.
namespace BootTrace {
enum Stage : uint8_t {
  Entry, ButtonObserved, SettingsReady, AudioReady, FirstBlock,
  FilesystemReady, IntentResolved, FirstCheckpoint, DisplayReady, UiReady, Count
};
inline std::atomic<uint32_t> *stamps() {
  static std::atomic<uint32_t> values[Count]{};
  return values;
}
inline void mark(Stage stage) {
#if defined(ARDUINO_ARCH_ESP32)
  uint32_t expected = 0;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  stamps()[stage].compare_exchange_strong(expected, now ? now : 1);
#else
  (void)stage;
#endif
}
inline uint32_t get(Stage stage) { return stamps()[stage].load(); }
} // namespace BootTrace
