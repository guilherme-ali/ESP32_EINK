#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_INTERNAL = 1;
constexpr uint32_t MALLOC_CAP_DMA = 2;
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
