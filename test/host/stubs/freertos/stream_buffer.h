#pragma once
#include "task.h"
#include <cstring>

namespace HostRTOS {
struct StreamBuffer {
  uint8_t *storage;
  size_t capacity, head = 0, tail = 0, used = 0;
  StaticStreamBuffer_t *owner;
  std::mutex mutex;
  std::condition_variable readable, writable;
};
inline std::atomic<size_t> streamCreates{0}, streamDeletes{0}, streamResets{0};
inline std::atomic<size_t> streamSends{0}, streamReceives{0}, sentBytes{0}, receivedBytes{0};
inline std::atomic<size_t> streamHighWater{0};
inline bool failStreamCreate = false;
inline void resetStreams() {
  streamCreates = streamDeletes = streamResets = 0;
  streamSends = streamReceives = sentBytes = receivedBytes = streamHighWater = 0;
  failStreamCreate = false;
}
} // namespace HostRTOS

inline StreamBufferHandle_t xStreamBufferCreateStatic(size_t capacity, size_t trigger,
    uint8_t *storage, StaticStreamBuffer_t *owner) {
  if (HostRTOS::failStreamCreate || !capacity || !trigger || trigger > capacity || !storage || !owner) return nullptr;
  auto *buffer = new HostRTOS::StreamBuffer;
  buffer->storage = storage; buffer->capacity = capacity; buffer->owner = owner;
  owner->handle = buffer; ++HostRTOS::streamCreates;
  return buffer;
}
inline size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t buffer) {
  std::lock_guard<std::mutex> lock(buffer->mutex);
  return buffer->capacity - buffer->used;
}
inline size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer) {
  std::lock_guard<std::mutex> lock(buffer->mutex);
  return buffer->used;
}
inline BaseType_t xStreamBufferIsEmpty(StreamBufferHandle_t buffer) {
  return xStreamBufferBytesAvailable(buffer) == 0 ? pdTRUE : pdFALSE;
}
inline size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *input, size_t count, TickType_t ticks) {
  ++HostRTOS::streamSends;
  std::unique_lock<std::mutex> lock(buffer->mutex);
  if (ticks && buffer->capacity - buffer->used < count)
    buffer->writable.wait_for(lock, HostRTOS::waitDuration(ticks), [buffer, count] {
      return buffer->capacity - buffer->used >= count;
    });
  size_t n = std::min(count, buffer->capacity - buffer->used);
  auto *bytes = static_cast<const uint8_t *>(input);
  size_t first = std::min(n, buffer->capacity - buffer->head);
  std::memcpy(buffer->storage + buffer->head, bytes, first);
  std::memcpy(buffer->storage, bytes + first, n - first);
  buffer->head = (buffer->head + n) % buffer->capacity; buffer->used += n;
  size_t high = HostRTOS::streamHighWater.load();
  while (high < buffer->used && !HostRTOS::streamHighWater.compare_exchange_weak(high, buffer->used)) {}
  HostRTOS::sentBytes += n;
  lock.unlock(); buffer->readable.notify_one();
  return n;
}
inline size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *output, size_t count, TickType_t ticks) {
  ++HostRTOS::streamReceives;
  std::unique_lock<std::mutex> lock(buffer->mutex);
  if (ticks && !buffer->used)
    buffer->readable.wait_for(lock, HostRTOS::waitDuration(ticks), [buffer] { return buffer->used > 0; });
  size_t n = std::min(count, buffer->used);
  auto *bytes = static_cast<uint8_t *>(output);
  size_t first = std::min(n, buffer->capacity - buffer->tail);
  std::memcpy(bytes, buffer->storage + buffer->tail, first);
  std::memcpy(bytes + first, buffer->storage, n - first);
  buffer->tail = (buffer->tail + n) % buffer->capacity; buffer->used -= n;
  HostRTOS::receivedBytes += n;
  lock.unlock(); buffer->writable.notify_one();
  return n;
}
inline BaseType_t xStreamBufferReset(StreamBufferHandle_t buffer) {
  std::lock_guard<std::mutex> lock(buffer->mutex);
  buffer->head = buffer->tail = buffer->used = 0; ++HostRTOS::streamResets;
  return pdPASS;
}
inline void vStreamBufferDelete(StreamBufferHandle_t buffer) {
  if (!buffer) return;
  buffer->owner->handle = nullptr; ++HostRTOS::streamDeletes;
  delete buffer;
}
