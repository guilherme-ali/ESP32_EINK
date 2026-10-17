#pragma once
#include "platform.h"
#include <atomic>

// Owner: mainApp, with start/snapshot/active/acknowledgeFinished serialized on
// the UI task. release/cancel are thread safe. Keep this object alive until ACK.
class AssistantJob {
public:
  using ConnectFn = bool (*)();
  enum class State { Idle, Connecting, Listening, Thinking, Speaking, Done, Failed };
  struct Snapshot {
    State state = State::Idle;
    char detail[96] = {};
    char text[1024] = {};
    char error[256] = {};
    uint32_t capturedMs = 0;
    uint32_t receivedAudioMs = 0, playedAudioMs = 0, rebufferCount = 0;
    bool bufferedAudio = false;
    bool released = false, finished = false;
  };
  AssistantJob() = default;
  AssistantJob(const AssistantJob &) = delete;
  AssistantJob &operator=(const AssistantJob &) = delete;
  bool start(const Settings &, AudioCodec &, ConnectFn);
  void release();
  void cancel();
  bool active() const;
  bool snapshot(Snapshot &) const;
  bool acknowledgeFinished();
private:
  struct Impl;
  Impl *impl_ = nullptr;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  Snapshot view_;
  void reap();
  void status(State, const char *detail, const char *error = nullptr);
  void appendText(const char *text);
  static void audioTask(void *);
  static void networkTask(void *);
  static void gatewayTask(void *);
};
