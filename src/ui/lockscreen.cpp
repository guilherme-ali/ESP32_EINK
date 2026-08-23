#include "lockscreen.h"
#include "wallpapers.h"
#include <string.h>

void LockScreen::draw(int wallpaperIndex) {
  if (wallpaperIndex >= 0 && wallpaperIndex < WALLPAPER_COUNT) {
    memcpy_P(epd_.buffer(), WALLPAPERS[wallpaperIndex], epd_.bufferLen());
  } else {
    canvas_.clear(EPD_WHITE);
  }
  epd_.display();
}
