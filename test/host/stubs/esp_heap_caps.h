#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
inline void *heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void heap_caps_free(void *ptr) { std::free(ptr); }
