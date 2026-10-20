#pragma once
// Dedicated runtime test adapter. Production includes the actual SDK instead.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Settings {
  bool assistantEnabled = true, geminiFreeConfirmed = true;
  char assistantModel[96] = "gemini-3.8-live";
  char assistantGatewayUrl[256] = "";
  char assistantGatewayToken[129] = "";
  char assistantTimeZone[64] = "America/Sao_Paulo";
  char sttApiKey[128] = "test_API_key";
  float micGainDb = 24, speakerVolumePercent = 20;
};
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
void portENTER_CRITICAL(portMUX_TYPE *);
void portEXIT_CRITICAL(portMUX_TYPE *);
enum eTaskState { eRunning, eSuspended };
struct AssistantHostTask;
using TaskHandle_t = AssistantHostTask *;
constexpr int pdPASS = 1;
inline unsigned pdMS_TO_TICKS(unsigned n) { return n; }
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
eTaskState eTaskGetState(TaskHandle_t);
void vTaskDelay(unsigned);
void vTaskSuspend(TaskHandle_t);
void vTaskDelete(TaskHandle_t);
uint32_t millis();
uint64_t assistantMonotonicUs();
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
struct AssistantHostSerial { void printf(const char *, ...); };
extern AssistantHostSerial Serial;
void esp_fill_random(void *, size_t);
int mbedtls_sha1_ret(const unsigned char *, size_t, unsigned char *);
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
void *heap_caps_malloc(size_t, unsigned);

class AudioCodec {
public:
  bool prepareCapture(uint32_t, float);
  bool preparePlayback(uint32_t, float, bool boundedSpeech = false);
  void end();
  bool setSampleRate(uint32_t);
  bool enable(bool);
  bool setMicGain(float) { return true; }
  bool setVolume(float) { return true; }
  float volumePercent() const;
  bool readRegister(uint8_t, uint16_t &);
  bool read(void *, size_t, size_t *, uint32_t);
  bool write(const void *, size_t, size_t *, uint32_t);
};
// Dedicated boundary fake: the session owns a producer and returns mono PCM
// already converted to the requested rate, not another reader of AudioCodec.
class CaptureSession {
public:
  ~CaptureSession() { end(); }
  bool start(AudioCodec &, uint32_t, float);
  bool setOutputRate(uint32_t);
  bool read(int16_t *, size_t, size_t *, uint32_t timeoutMs = 100);
  void requestStop();
  bool finished() const { return finished_.load(); }
  bool drained() const;
  bool failed() const;
  bool active() const { return !finished_.load() && !ended_.load(); }
  uint32_t overflowCount() const;
  uint32_t inputRate() const { return inputRate_; }
  void end();
private:
  AudioCodec *codec_ = nullptr;
  std::thread producer_;
  mutable std::mutex mutex_;
  std::vector<int16_t> pcm_;
  size_t cursor_ = 0;
  uint32_t inputRate_ = 0;
  bool readStarted_ = false, outputReady_ = false;
  std::atomic<bool> stop_{false}, finished_{false}, ended_{true};
};
struct HttpResponse {
  int statusCode = 0;
  bool headersComplete = false, bodyComplete = false, bodyTruncated = false;
  std::string body, location;
};
class NetworkClient {
public:
  struct Wire;
  std::shared_ptr<Wire> wire;
  ~NetworkClient() { stop(); }
  void setInsecure() {}
  void setTimeout(unsigned) {}
  void setHandshakeTimeout(unsigned) {}
  bool isPhone() const;
  int connect(const char *, uint16_t, int32_t);
  size_t write(const uint8_t *, size_t);
  int read();
  int available();
  bool connected();
  void stop();
};
class HttpClient {
public:
  static bool readResponse(NetworkClient &, HttpResponse &, size_t, uint32_t);
};
namespace NetworkRoute { bool phoneSelected(); bool diagnosticEarlyPlayback(); }
namespace AssistantHost {
extern std::mutex mutex;
extern std::vector<std::string> jsonSent, requests, hosts;
extern std::vector<uint8_t> playback;
extern std::atomic<unsigned> captureReads, writes, tasksAlive, socketsAlive, rateChanges;
extern std::atomic<bool> codecOn, captureInRead, clockRace, callbackEntered, callbackExit;
extern std::atomic<int> failTaskAt, taskCount, connectDelay, allocFailAt, allocCount;
extern std::atomic<bool> failRead, failWrite, wrongAccept, networkUnavailable;
extern std::atomic<uint32_t> timeOffset;
extern std::string afterSetup, afterEnd, afterToolResponse, redirectUrl;
extern std::atomic<unsigned> gatewayDelay;
extern std::atomic<bool> gatewayBadJson;
extern std::atomic<bool> redirectForever;
extern std::atomic<bool> gatewayLoseFirst;
extern std::string gatewayBody;
extern std::atomic<bool> phoneRoute, releaseResponseTail, responseHeadRead, fastTaskYield;
extern std::atomic<bool> earlyPlayback;
extern std::string delayedAfterEnd;
extern std::atomic<size_t> playbackWriteLimit;
extern std::atomic<size_t> failPlaybackAfter, pausePlaybackAfter;
extern std::atomic<bool> playbackPaused;
extern std::atomic<unsigned> audioStackBytes;
extern std::string serialOutput;
extern std::atomic<unsigned> closeDelayMs;
extern std::atomic<bool> closingSocket, writeDuringClose;
extern std::vector<int16_t> sessionPrefix, sessionTail;
extern std::atomic<unsigned> capturePreparations, playbackPreparations, sessionReads, sessionEnds;
extern std::atomic<unsigned> sessionStopRequests;
extern std::atomic<bool> sessionProducerAlive, sessionStopRequested, sessionReadAfterEnd;
extern std::atomic<bool> sessionFail, sessionOverflow, sessionRejectRate, failPrepareCapture, failPreparePlayback;
extern std::atomic<bool> playbackBeforeSessionEnd;
extern std::atomic<uint32_t> sessionOutputRate;
extern std::atomic<float> playbackVolume;
extern std::string wireAfterSetup, wireAfterEnd, wireAfterToolResponse;
extern std::atomic<bool> eofAfterSetup, eofAfterEnd, eofAfterToolResponse;
extern std::atomic<bool> wireTailRead;
void reset();
void sleep(unsigned ms);
std::vector<std::string> messages();
bool connectCallback();
}
