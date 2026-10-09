#pragma once
#include "Arduino.h"
#include <map>
#include <set>
#include <variant>

namespace Host {
using NvsValue = std::variant<std::string, uint8_t, int8_t, uint16_t, uint32_t, float>;
inline std::map<std::string, std::map<std::string, NvsValue>> nvs;
inline std::set<std::string> failPuts; // namespace/key, persistent until cleared
inline bool failNvsBegin = false;
inline unsigned nvsEpoch = 1;
inline void rebootNvs() { ++nvsEpoch; }
inline void resetNvs() { nvs.clear(); failPuts.clear(); failNvsBegin = false; rebootNvs(); }
template <typename T> void seedNvs(const char *key, T value, const char *ns = "cfg") { nvs[ns][key] = value; }
}

// Arduino-ESP32 contract: missing/oversized strings leave char buffers intact;
// empty putString returns 0 even on success. Bool uses the UChar NVS type.
class Preferences {
  std::string namespace_;
  unsigned epoch_ = 0;
  bool readOnly_ = false;
  bool active() const { return epoch_ == Host::nvsEpoch; }
  const Host::NvsValue *find(const char *key) const {
    if (!active() || !key) return nullptr;
    auto ns = Host::nvs.find(namespace_);
    if (ns == Host::nvs.end()) return nullptr;
    auto value = ns->second.find(key);
    return value == ns->second.end() ? nullptr : &value->second;
  }
  template <typename T> T get(const char *key, const T &fallback) const {
    const auto *v = find(key); const auto *typed = v ? std::get_if<T>(v) : nullptr;
    return typed ? *typed : fallback;
  }
  template <typename T> size_t put(const char *key, T value, size_t count = sizeof(T)) {
    if (!active() || readOnly_ || !key || std::strlen(key) > 15 ||
        Host::failPuts.count(namespace_ + "/" + key)) return 0;
    Host::nvs[namespace_][key] = value; return count;
  }
public:
  bool begin(const char *name, bool readOnly = false) {
    if (active() || Host::failNvsBegin || !name || std::strlen(name) > 15) return false;
    namespace_ = name; readOnly_ = readOnly; epoch_ = Host::nvsEpoch; return true;
  }
  void end() { epoch_ = 0; }
  bool isKey(const char *key) const { return find(key) != nullptr; }
  bool remove(const char *key) {
    if (!active() || readOnly_ || !key || Host::failPuts.count(namespace_ + "/" + key)) return false;
    return Host::nvs[namespace_].erase(key) != 0;
  }
  String getString(const char *key, const String &fallback = "") const {
    return String(get<std::string>(key, fallback.c_str()));
  }
  size_t getString(const char *key, char *out, size_t capacity) const {
    auto v = find(key); auto text = v ? std::get_if<std::string>(v) : nullptr;
    if (!text || !out || text->size() >= capacity) return 0;
    std::memcpy(out, text->c_str(), text->size() + 1); return text->size() + 1;
  }
  size_t putString(const char *key, const String &value) {
    return put(key, std::string(value.c_str()), value.length());
  }
  uint8_t getUChar(const char *key, uint8_t v = 0) const { return get(key, v); }
  int8_t getChar(const char *key, int8_t v = 0) const { return get(key, v); }
  uint16_t getUShort(const char *key, uint16_t v = 0) const { return get(key, v); }
  uint32_t getUInt(const char *key, uint32_t v = 0) const { return get(key, v); }
  float getFloat(const char *key, float v = 0) const { return get(key, v); }
  bool getBool(const char *key, bool v = false) const { return getUChar(key, v ? 1 : 0) == 1; }
  size_t putUChar(const char *key, uint8_t v) { return put(key, v); }
  size_t putChar(const char *key, int8_t v) { return put(key, v); }
  size_t putUShort(const char *key, uint16_t v) { return put(key, v); }
  size_t putUInt(const char *key, uint32_t v) { return put(key, v); }
  size_t putFloat(const char *key, float v) { return put(key, v); }
  size_t putBool(const char *key, bool v) { return putUChar(key, v ? 1 : 0); }
};
