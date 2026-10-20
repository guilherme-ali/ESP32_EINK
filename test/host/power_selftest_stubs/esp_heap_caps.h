#pragma once
#include "../stubs/esp_heap_caps.h"
constexpr unsigned MALLOC_CAP_INTERNAL = 4;
inline size_t heap_caps_get_free_size(unsigned) { return 0; }
