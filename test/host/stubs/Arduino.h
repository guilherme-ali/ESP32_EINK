#pragma once

// Small byte-oriented subset of Arduino String; no production parser lives here.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cstdarg>
#include <string>
#include <type_traits>

constexpr int HEX = 16;
constexpr int DEC = 10;
using std::min;
using std::max;

class String {
  std::string value_;

public:
  String() = default;
  String(const char *s) : value_(s ? s : "") {}
  String(const std::string &s) : value_(s) {}
  String(char c) : value_(1, c) {}
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  explicit String(T n, int base = DEC) {
    if (base == HEX) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(n));
      value_ = buffer;
    } else value_ = std::to_string(n);
  }
  const char *c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool isEmpty() const { return value_.empty(); }
  bool reserve(size_t n) { value_.reserve(n); return true; }
  bool concat(const char *s, size_t n) {
    if (!s) return false;
    value_.append(s, n); return true;
  }
  bool concat(const char *s) { return s && concat(s, std::strlen(s)); }
  bool concat(char c) { value_ += c; return true; }
  bool concat(const String &s) { return concat(s.c_str(), s.length()); }
  String &operator+=(const String &s) { value_ += s.value_; return *this; }
  String &operator+=(const char *s) { if (s) value_ += s; return *this; }
  String &operator+=(char c) { value_ += c; return *this; }
  char operator[](size_t i) const { return i < length() ? value_[i] : '\0'; }
  bool operator==(const String &s) const { return value_ == s.value_; }
  bool operator!=(const String &s) const { return !(*this == s); }
  friend String operator+(const String &a, const String &b) { return String(a.value_ + b.value_); }
  int indexOf(char c, size_t start = 0) const { return position(value_.find(c, start)); }
  int indexOf(const char *s, size_t start = 0) const { return position(value_.find(s, start)); }
  int indexOf(const String &s, size_t start = 0) const { return position(value_.find(s.value_, start)); }
  int lastIndexOf(char c) const { return position(value_.rfind(c)); }
  bool startsWith(const String &s) const { return value_.compare(0, s.length(), s.value_) == 0; }
  bool endsWith(const String &s) const {
    return length() >= s.length() && value_.compare(length() - s.length(), s.length(), s.value_) == 0;
  }
  String substring(size_t begin) const { return substring(begin, length()); }
  String substring(size_t begin, size_t end) const {
    if (begin > end) std::swap(begin, end);
    if (begin >= length()) return "";
    return String(value_.substr(begin, std::min(end, length()) - begin));
  }
  void remove(size_t begin) { if (begin < length()) value_.erase(begin); }
  void remove(size_t begin, size_t count) { if (begin < length()) value_.erase(begin, count); }
  void trim() {
    auto space = [](unsigned char c) { return std::isspace(c); };
    auto begin = std::find_if_not(value_.begin(), value_.end(), space);
    auto end = std::find_if_not(value_.rbegin(), value_.rend(), space).base();
    value_ = begin < end ? std::string(begin, end) : std::string();
  }
  void toLowerCase() {
    for (char &c : value_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  bool equalsIgnoreCase(const String &s) const {
    if (length() != s.length()) return false;
    for (size_t i = 0; i < length(); ++i)
      if (std::tolower(static_cast<unsigned char>(value_[i])) !=
          std::tolower(static_cast<unsigned char>(s[i]))) return false;
    return true;
  }
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }

private:
  static int position(size_t n) { return n == std::string::npos ? -1 : static_cast<int>(n); }
};

inline size_t strlcpy(char *out, const char *in, size_t capacity) {
  size_t n = std::strlen(in);
  if (capacity) { size_t copied = std::min(n, capacity - 1); std::memcpy(out, in, copied); out[copied] = '\0'; }
  return n;
}
struct HostSerial { void printf(const char *, ...) {} };
inline HostSerial Serial;

namespace Host {
inline uint32_t now = 0;
inline void resetClock(uint32_t value = 0) { now = value; }
}
inline uint32_t millis() { return Host::now; }
inline void delay(uint32_t ms) { Host::now += ms; }
inline void yield() { delay(1); }
inline long random(long lower, long upper) { return upper > lower ? lower + (upper - lower) / 2 : lower; }
inline long random(long upper) { return random(0, upper); }
template <typename T> inline T constrain(T value, T low, T high) {
  return std::min(std::max(value, low), high);
}
