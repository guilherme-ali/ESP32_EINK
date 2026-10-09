#pragma once
#include "FS.h"

class HostLittleFS : public fs::FS {
public:
  uint64_t capacity = 4 * 1024 * 1024;
  uint64_t totalBytes() const { return capacity; }
  void reset() { fs::FS::clear(); capacity = 4 * 1024 * 1024; }
};
inline HostLittleFS LittleFS;
