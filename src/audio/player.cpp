#include "player.h"
#include "wav.h"
#include <LittleFS.h>
#include <string.h>

namespace {
constexpr int kChunkFrames = 256;
int16_t monoBuf[kChunkFrames];
int16_t stereoBuf[kChunkFrames * 2];
} // namespace

bool Player::play(AudioCodec &codec, const char *path) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;

  WavHeader hdr;
  if (file.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) {
    file.close();
    return false;
  }
  if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) {
    file.close();
    return false;
  }

  if (!codec.setSampleRate(hdr.sampleRate)) {
    file.close();
    return false;
  }
  codec.enable(true);

  while (file.available()) {
    int monoRead = file.read((uint8_t *)monoBuf, sizeof(monoBuf)) / sizeof(int16_t);
    if (monoRead <= 0) break;

    for (int i = 0; i < monoRead; i++) {
      stereoBuf[i * 2] = monoBuf[i];
      stereoBuf[i * 2 + 1] = monoBuf[i];
    }

    size_t written = 0;
    codec.write(stereoBuf, monoRead * 2 * sizeof(int16_t), &written);
  }

  file.close();
  return true;
}
