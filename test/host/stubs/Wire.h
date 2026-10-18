#pragma once
#include <cstdint>
#include <cstddef>

class TwoWire {
public:
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t *, size_t len) { return len; }
  uint8_t endTransmission(bool = true) { return 0; }
  int requestFrom(int, int len) { return len; }
  int read() { return 0; }
};

inline TwoWire Wire;
