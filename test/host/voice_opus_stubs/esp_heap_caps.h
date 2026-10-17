#pragma once
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT 1U
#define MALLOC_CAP_SPIRAM 2U
#define MALLOC_CAP_INTERNAL 4U

extern "C" void *heap_caps_malloc(size_t bytes, uint32_t caps);
extern "C" void heap_caps_free(void *pointer);
