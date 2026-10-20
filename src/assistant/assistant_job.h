#pragma once
#include "platform.h"
#include <atomic>

class CaptureSession;

// Owner: mainApp, with start/snapshot/active/acknowledgeFinished serialized on
// the UI task. release/cancel are thread safe. Keep this object alive until ACK.
class AssistantJob {
public:
  using ConnectFn = bool (*)();
  enum class State { Idle, Connecting, Listening, Thinking, Speaking, Done, Failed };
  struct Telemetry {
    char media[8] = "legacy";
    uint32_t mediaSentBytes = 0, mediaReceivedBytes = 0;
    uint32_t encodeUs = 0, decodeUs = 0, networkStackFreeBytes = 0;
    uint32_t turnId = 0;
    uint32_t routeBeginUs = 0, routeReadyUs = 0, networkConnectUs = 0, wsUpgradeUs = 0;
    uint32_t setupCompleteUs = 0, captureDoneUs = 0, firstInputUs = 0, lastInputUs = 0;
    uint32_t activityEndUs = 0, firstAudioMessageUs = 0, firstPcmUs = 0;
    uint32_t turnCompleteUs = 0, responseReadyUs = 0, firstI2sUs = 0, lastI2sUs = 0, networkDoneUs = 0;
    uint32_t backlogAtRelease = 0, maxBacklogBytes = 0, inputBytes = 0, releaseUs = 0;
    uint8_t releaseSource = 0; // 1 physical button, 2 serial/benchmark, 3 capture limit
    bool routeKnown = false, phoneRoute = false;
  };
  struct Snapshot {
    State state = State::Idle;
    char detail[96] = {};
    char text[1024] = {};
    char error[256] = {};
    uint32_t capturedMs = 0;
    uint32_t receivedAudioMs = 0, playedAudioMs = 0, rebufferCount = 0;
    bool bufferedAudio = false;
    bool released = false, finished = false;
    Telemetry telemetry;
  };
  AssistantJob() = default;
  AssistantJob(const AssistantJob &) = delete;
  AssistantJob &operator=(const AssistantJob &) = delete;
  // An adopted session is borrowed, never deleted. Except when already busy,
  // start takes responsibility for end(), including failures; keep it alive
  // and do not reuse it until finished. release stops ingress and drains the
  // accepted prefix.
  bool start(const Settings &, AudioCodec &, ConnectFn, CaptureSession *capture = nullptr);
  void release(bool physicalButton = false);
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
