#pragma once
#include <Arduino.h>
namespace HostPowerTest {
inline int64_t timerStepUs = 0, timerUs = 0;
}
inline int64_t esp_timer_get_time() {
  HostPowerTest::timerUs += HostPowerTest::timerStepUs;
  return HostPowerTest::timerUs + static_cast<int64_t>(Host::now) * 1000;
}
