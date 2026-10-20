#pragma once
#include <atomic>
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
namespace HostRecorderHeap {
inline std::atomic<size_t> calls{0}, bytes{0};
inline std::atomic<bool> fail{false};
inline void reset() { calls = bytes = 0; fail = false; }
}
inline void *heap_caps_malloc(size_t size, unsigned) {
  ++HostRecorderHeap::calls;
  HostRecorderHeap::bytes += size;
  return HostRecorderHeap::fail.load() ? nullptr : std::malloc(size);
}
inline void heap_caps_free(void *ptr) { std::free(ptr); }
