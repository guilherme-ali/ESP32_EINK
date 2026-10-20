#include "../../src/audio/playback_level.h"
#include <cassert>
#include <cstdio>
#include <cmath>
int main() {
  PlaybackLevel level;
  assert(level.rmsDbfs() == -160 && level.peakDbfs() == -160);
  const int16_t pcm[] = {16384, -16384, 0, 0};
  level.add(pcm, 4);
  assert(level.samples == 4 && level.peak == 16384 && level.fullScaleSamples == 0);
  assert(std::fabs(level.peakDbfs() + 6.020599913) < .00001);
  assert(std::fabs(level.rmsDbfs() + 9.030899870) < .00001);
  const int16_t limits[] = {-32768, 32767};
  level.add(limits, 2);
  assert(level.samples == 6 && level.peak == 32768 && level.fullScaleSamples == 2);
  PlaybackLevel full;
  full.add(limits, 2);
  assert(full.squares == 2147418113ULL && std::fabs(full.rmsDbfs()) < .001);
  PlaybackLevel windows;
  windows.windowSamples = 2;
  windows.add(pcm, 4);
  assert(windows.fullWindows == 2 && std::fabs(windows.maxWindowDbfs() + 6.020599913) < .00001);
  std::puts("Playback source meter: count, RMS, peak, INT16_MIN and silence OK");
}
