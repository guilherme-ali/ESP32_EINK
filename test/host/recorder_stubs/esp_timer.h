#pragma once
#include <chrono>
#include <cstdint>

// Monotonico real, independente do clock Arduino fake e dos delays RTOS
// acelerados. EOF controlado nao pode fabricar dois segundos por iteracao.
inline int64_t esp_timer_get_time() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
