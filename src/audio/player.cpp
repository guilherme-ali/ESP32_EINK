#include "player.h"
#include "wav.h"
#include "playback_level.h"
#include "speech_leveler.h"
#include <LittleFS.h>
#include "../storage/note_files.h"
#include <string.h>

namespace {
constexpr int kChunkFrames = 256;
int16_t monoBuf[kChunkFrames];
int16_t stereoBuf[kChunkFrames * 2];
} // namespace

bool Player::play(AudioCodec &codec, const char *path) {
  struct Cleanup {
    AudioCodec &codec;
    ~Cleanup() { codec.end(); }
  } cleanup{codec};
  File file = NoteFiles::openRead(path);
  if (!file) return false;

  WavHeader hdr;
  if (file.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) {
    file.close();
    return false;
  }
  if (!validWavHeader(hdr, file.size())) {
    file.close();
    return false;
  }

  SpeechLeveler leveler;
  if (!leveler.configure(hdr.sampleRate) ||
      !codec.preparePlayback(hdr.sampleRate, codec.volumePercent(), true)) {
    file.close();
    return false;
  }

  auto writeAll = [&codec](const void *data, size_t bytes) {
    size_t offset = 0;
    const uint32_t begin = millis();
    while (offset < bytes) {
      size_t written = 0;
      if (!codec.write((const uint8_t *)data + offset, bytes - offset, &written, 40) ||
          !written || written > bytes - offset || uint32_t(millis() - begin) >= 2000) return false;
      offset += written;
    }
    return true;
  };
  bool ok = true;
#if ARDUINO_USB_CDC_ON_BOOT
  PlaybackLevel sourceLevel;
  sourceLevel.windowSamples = hdr.sampleRate / 10;
  PlaybackLevel processedLevel;
  processedLevel.windowSamples = hdr.sampleRate / 10;
#endif
  while (file.available()) {
    size_t bytesRead = file.read((uint8_t *)monoBuf, sizeof(monoBuf));
    if (!bytesRead || bytesRead % sizeof(int16_t)) { ok = false; break; }
    int monoRead = bytesRead / sizeof(int16_t);
#if ARDUINO_USB_CDC_ON_BOOT
    sourceLevel.add(monoBuf, static_cast<size_t>(monoRead));
#endif

    for (int i = 0; i < monoRead; i++) {
      monoBuf[i] = leveler.process(monoBuf[i]);
      stereoBuf[i * 2] = monoBuf[i];
      stereoBuf[i * 2 + 1] = monoBuf[i];
    }
#if ARDUINO_USB_CDC_ON_BOOT
    processedLevel.add(monoBuf, static_cast<size_t>(monoRead));
#endif

    if (!writeAll(stereoBuf, monoRead * 2 * sizeof(int16_t))) { ok = false; break; }
  }

  // Pay the lookahead once, then drain its real PCM tail before the DMA silence.
  while (ok) {
    size_t frames = 0;
    while (frames < kChunkFrames && leveler.flush(monoBuf[frames])) {
      stereoBuf[frames * 2] = stereoBuf[frames * 2 + 1] = monoBuf[frames];
      ++frames;
    }
    if (!frames) break;
#if ARDUINO_USB_CDC_ON_BOOT
    processedLevel.add(monoBuf, frames);
#endif
    ok = writeAll(stereoBuf, frames * 2 * sizeof(int16_t));
  }

  // Push one DMA ring of silence so end() cannot truncate the WAV tail.
  // codec.cpp uses 8 descriptors of 512 stereo frames on the legacy I2S API.
  memset(stereoBuf, 0, sizeof(stereoBuf));
  for (int chunk = 0; ok && chunk < 8 * 512 / kChunkFrames; ++chunk) {
    ok = writeAll(stereoBuf, sizeof(stereoBuf));
  }
  file.close();
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("[PlaybackSource] kind=wav scope=offered samples=%llu rms_dbfs=%.3f max100ms_dbfs=%.3f peak_dbfs=%.3f fullscale=%lu volume_cached=%.1f\n",
                (unsigned long long)sourceLevel.samples, sourceLevel.rmsDbfs(), sourceLevel.maxWindowDbfs(),
                sourceLevel.peakDbfs(), (unsigned long)sourceLevel.fullScaleSamples, codec.volumePercent());
  Serial.printf("[PlaybackProcessed] kind=wav scope=offered samples=%llu rms_dbfs=%.3f max100ms_dbfs=%.3f peak_dbfs=%.3f latency_samples=%u guards=%llu\n",
                (unsigned long long)processedLevel.samples, processedLevel.rmsDbfs(), processedLevel.maxWindowDbfs(),
                processedLevel.peakDbfs(), static_cast<unsigned>(leveler.latencySamples()),
                (unsigned long long)leveler.hardGuardSamples());
#endif
  return ok;
}
