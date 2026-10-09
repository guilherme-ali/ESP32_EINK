#pragma once
#include "Arduino.h"

namespace Host {
// NOT MD5/cryptography: FNV-1a 32-bit repeated four times as 32 lowercase hex
// characters. Known vector "abc" => 1a47e90b1a47e90b1a47e90b1a47e90b.
inline uint32_t hashStep(uint32_t h, uint8_t c) { return (h ^ c) * 16777619U; }
inline String hashString(uint32_t h) {
  char word[9]; std::snprintf(word, sizeof(word), "%08x", static_cast<unsigned>(h));
  return String(word) + word + word + word;
}
inline String mockFileHash(const std::string &bytes) {
  uint32_t h = 2166136261U;
  for (unsigned char c : bytes) h = hashStep(h, c);
  return hashString(h);
}
}
class MD5Builder {
  uint32_t hash_ = 2166136261U;
public:
  void begin() { hash_ = 2166136261U; }
  void add(uint8_t *bytes, uint16_t length) {
    for (uint16_t i = 0; i < length; ++i) hash_ = Host::hashStep(hash_, bytes[i]);
  }
  void calculate() {}
  String toString() const { return Host::hashString(hash_); }
};
