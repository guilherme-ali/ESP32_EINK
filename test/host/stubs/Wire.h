#pragma once
#include <cstdint>
#include <cstddef>

class TwoWire {
public:
#ifdef AUDIO_CODEC_TEST_HOOKS
  bool beginSuccess = true;
  int sda = -1, scl = -1;
  unsigned begins = 0;
  int failWriteReg = -1, failReadReg = -1;
  uint8_t registers[256] = {};
  unsigned writes = 0;
  bool begin(int sdaPin, int sclPin) { ++begins; sda = sdaPin; scl = sclPin; return beginSuccess; }
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t reg) { reg_ = reg; return 1; }
  size_t write(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      if (failWriteReg != reg_) registers[reg_] = data[i];
      ++writes;
    }
    return len;
  }
  uint8_t endTransmission(bool = true) { return failWriteReg == reg_ ? 1 : 0; }
  int requestFrom(int, int len) { return failReadReg == reg_ ? 0 : len; }
  int read() { return registers[reg_]; }
private:
  uint8_t reg_ = 0;
#else
  bool begin(int, int) { return true; }
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t *, size_t len) { return len; }
  uint8_t endTransmission(bool = true) { return 0; }
  int requestFrom(int, int len) { return len; }
  int read() { return 0; }
#endif
};

inline TwoWire Wire;
