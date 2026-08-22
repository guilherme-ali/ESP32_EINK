#include "buttons.h"

namespace {
uint8_t bootPin_ = 0;
uint8_t pwrPin_ = 0;
ButtonEventFn onEvent_ = nullptr;

uint8_t readLevel(uint8_t id) {
  uint8_t pin = (id == (uint8_t)BtnId::Boot) ? bootPin_ : pwrPin_;
  return digitalRead(pin);
}

void onSingleClick(Button *btn) {
  if (onEvent_) onEvent_((BtnId)btn->button_id, BtnAction::ShortClick);
}
void onDoubleClick(Button *btn) {
  if (onEvent_) onEvent_((BtnId)btn->button_id, BtnAction::DoubleClick);
}
void onLongPressStart(Button *btn) {
  if (onEvent_) onEvent_((BtnId)btn->button_id, BtnAction::LongPress);
}
void onPressUp(Button *btn) {
  if (onEvent_) onEvent_((BtnId)btn->button_id, BtnAction::Released);
}
} // namespace

void Buttons::begin(uint8_t bootPin, uint8_t pwrPin, ButtonEventFn onEvent) {
  bootPin_ = bootPin;
  pwrPin_ = pwrPin;
  onEvent_ = onEvent;

  pinMode(bootPin_, INPUT_PULLUP);
  pinMode(pwrPin_, INPUT_PULLUP);

  button_init(&boot_, readLevel, LOW, (uint8_t)BtnId::Boot);
  button_attach(&boot_, BTN_SINGLE_CLICK, onSingleClick);
  button_attach(&boot_, BTN_LONG_PRESS_START, onLongPressStart);
  button_attach(&boot_, BTN_PRESS_UP, onPressUp);
  button_start(&boot_);

  button_init(&pwr_, readLevel, LOW, (uint8_t)BtnId::Pwr);
  button_attach(&pwr_, BTN_SINGLE_CLICK, onSingleClick);
  button_attach(&pwr_, BTN_DOUBLE_CLICK, onDoubleClick);
  button_attach(&pwr_, BTN_LONG_PRESS_START, onLongPressStart);
  button_attach(&pwr_, BTN_PRESS_UP, onPressUp);
  button_start(&pwr_);

  lastTickMs_ = millis();
}

void Buttons::setCallback(ButtonEventFn onEvent) {
  onEvent_ = onEvent;
}

void Buttons::poll() {
  uint32_t now = millis();
  while (now - lastTickMs_ >= TICKS_INTERVAL) {
    button_ticks();
    lastTickMs_ += TICKS_INTERVAL;
  }
}
