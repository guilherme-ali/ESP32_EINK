#pragma once
#include "FreeRTOS.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace HostRTOS {
// Sem millis()/delay()/yield() Arduino: o clock fake existente nao e atomico.
struct Task {
  std::string name;
  uint32_t stackBytes;
  UBaseType_t priority;
  BaseType_t core;
  std::atomic<bool> allowed{true}, started{false}, completed{false};
  std::atomic<size_t> delayCalls{0}, yieldCalls{0};
  std::thread thread;
};
struct TaskExit {};
inline std::mutex taskMutex;
inline std::condition_variable taskWake;
inline std::vector<std::unique_ptr<Task>> tasks;
inline std::map<std::string, bool> startAllowed;
inline std::string failTaskName;
inline size_t createCalls = 0, createFailures = 0;
inline std::atomic<size_t> activeTasks{0};
inline thread_local Task *currentTask = nullptr;

inline std::chrono::microseconds waitDuration(TickType_t ticks) {
  return std::chrono::microseconds(std::min<uint64_t>(2000, std::max<uint64_t>(100, ticks * uint64_t(100))));
}
inline void allowTask(const std::string &name, bool allowed = true) {
  std::lock_guard<std::mutex> lock(taskMutex);
  startAllowed[name] = allowed;
  for (auto &task : tasks) if (task->name == name) task->allowed = allowed;
  taskWake.notify_all();
}
inline bool completed(const std::string &name) {
  std::lock_guard<std::mutex> lock(taskMutex);
  for (auto it = tasks.rbegin(); it != tasks.rend(); ++it)
    if ((*it)->name == name) return (*it)->completed.load();
  return false;
}
inline size_t delays(const std::string &name) {
  std::lock_guard<std::mutex> lock(taskMutex);
  size_t count = 0;
  for (const auto &task : tasks) if (task->name == name) count += task->delayCalls.load();
  return count;
}
inline size_t yields(const std::string &name) {
  std::lock_guard<std::mutex> lock(taskMutex);
  size_t count = 0;
  for (const auto &task : tasks) if (task->name == name) count += task->yieldCalls.load();
  return count;
}
inline void joinAll() {
  // Chamador primeiro pede parada e libera gates; join nunca cancela a task.
  for (auto &task : tasks) if (task->thread.joinable()) task->thread.join();
}
inline void resetTasks() {
  if (activeTasks.load()) throw std::logic_error("resetTasks com task ativa");
  joinAll();
  std::lock_guard<std::mutex> lock(taskMutex);
  tasks.clear(); startAllowed.clear(); failTaskName.clear();
  createCalls = createFailures = 0;
}
} // namespace HostRTOS

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t stackBytes, void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core) {
  std::lock_guard<std::mutex> lock(HostRTOS::taskMutex);
  ++HostRTOS::createCalls;
  if (name == HostRTOS::failTaskName) {
    ++HostRTOS::createFailures;
    if (handle) *handle = nullptr;
    return pdFAIL;
  }
  auto task = std::make_unique<HostRTOS::Task>();
  task->name = name; task->stackBytes = stackBytes; task->priority = priority; task->core = core;
  auto gate = HostRTOS::startAllowed.find(name);
  if (gate != HostRTOS::startAllowed.end()) task->allowed = gate->second;
  auto *raw = task.get();
  if (handle) *handle = raw;
  ++HostRTOS::activeTasks;
  try {
    raw->thread = std::thread([raw, fn, arg] {
      HostRTOS::currentTask = raw;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      {
        std::unique_lock<std::mutex> lock(HostRTOS::taskMutex);
        HostRTOS::taskWake.wait(lock, [raw] { return raw->allowed.load(); });
      }
      raw->started = true;
      try { fn(arg); } catch (const HostRTOS::TaskExit &) {}
      --HostRTOS::activeTasks;
      raw->completed = true;
      HostRTOS::currentTask = nullptr;
    });
  } catch (const std::system_error &) {
    --HostRTOS::activeTasks; ++HostRTOS::createFailures;
    if (handle) *handle = nullptr;
    return pdFAIL;
  }
  HostRTOS::tasks.push_back(std::move(task));
  return pdPASS;
}
inline void vTaskDelay(TickType_t ticks) {
  if (HostRTOS::currentTask) ++HostRTOS::currentTask->delayCalls;
  std::this_thread::sleep_for(HostRTOS::waitDuration(ticks));
}
inline void taskYIELD() {
  if (HostRTOS::currentTask) ++HostRTOS::currentTask->yieldCalls;
  std::this_thread::yield();
}
inline void vTaskDelete(TaskHandle_t handle) {
  if (handle || !HostRTOS::currentTask)
    throw std::logic_error("stub vTaskDelete so suporta exclusao cooperativa da propria task");
  throw HostRTOS::TaskExit{};
}
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return HostRTOS::currentTask; }
