#pragma once
#include "buttons.h"

// Consume the entire overlapping gesture, not just PWR's PressDown. Otherwise
// BOOT's queued release can create a new recording after PWR cancelled it.
class ButtonChord {
public:
  bool consume(BtnId id, BtnAction action) {
    bool &down = id == BtnId::Boot ? boot_ : pwr_;
    if (action == BtnAction::PressDown) down = true;
    if (boot_ && pwr_) chord_ = true;
    const bool consumeEvent = chord_;
    if (action == BtnAction::ShortClick || action == BtnAction::LongPressRelease) down = false;
    if (!boot_ && !pwr_) chord_ = false;
    return consumeEvent;
  }
  void reset() { boot_ = pwr_ = chord_ = false; }
private:
  bool boot_ = false, pwr_ = false, chord_ = false;
};
