#pragma once
#include "Arduino.h"
constexpr int WL_CONNECTED = 3;
class IPAddress {
  uint32_t value_ = 0;
public:
  IPAddress() = default;
  IPAddress(uint32_t value) : value_(value) {}
  IPAddress &operator=(uint32_t value) { value_ = value; return *this; }
  operator uint32_t() const { return value_; }
};
struct TestWiFi {
  int state = WL_CONNECTED;
  int status() const { return state; }
};
inline TestWiFi WiFi;
