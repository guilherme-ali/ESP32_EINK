#include "buttons.h"

// A lib multi_button so emite BTN_SINGLE_CLICK depois de esperar
// SHORT_TICKS (300ms, ver multi_button.h) sem um segundo clique - e a
// janela de deteccao de duplo clique. Como nenhuma tela usa
// BTN_DOUBLE_CLICK, esses 300ms eram puro atraso em TODO clique. Em
// vez disso, sintetizamos o clique curto a partir de BTN_PRESS_UP (que
// dispara na hora que o botao solta, ver multi_button.c:163-169) e so
// ignoramos quando aquele aperto ja tiver virado long press.
namespace {
uint8_t bootPin_ = 0;
uint8_t pwrPin_ = 0;
ButtonEventFn onEvent_ = nullptr;
bool longFired_[3] = {false, false, false}; // indexado por BtnId (1=Boot, 2=Pwr)

uint8_t readLevel(uint8_t id) {
  uint8_t pin = (id == (uint8_t)BtnId::Boot) ? bootPin_ : pwrPin_;
  return digitalRead(pin);
}

void onPressDown(Button *btn) {
  longFired_[btn->button_id] = false;
}
void onLongPressStart(Button *btn) {
  longFired_[btn->button_id] = true;
  if (onEvent_) onEvent_((BtnId)btn->button_id, BtnAction::LongPress);
}
void onPressUp(Button *btn) {
  if (!longFired_[btn->button_id] && onEvent_) {
    onEvent_((BtnId)btn->button_id, BtnAction::ShortClick);
  }
}
} // namespace

void Buttons::begin(uint8_t bootPin, uint8_t pwrPin, ButtonEventFn onEvent) {
  bootPin_ = bootPin;
  pwrPin_ = pwrPin;
  onEvent_ = onEvent;

  pinMode(bootPin_, INPUT_PULLUP);
  pinMode(pwrPin_, INPUT_PULLUP);

  button_init(&boot_, readLevel, LOW, (uint8_t)BtnId::Boot);
  button_attach(&boot_, BTN_PRESS_DOWN, onPressDown);
  button_attach(&boot_, BTN_LONG_PRESS_START, onLongPressStart);
  button_attach(&boot_, BTN_PRESS_UP, onPressUp);
  button_start(&boot_);

  button_init(&pwr_, readLevel, LOW, (uint8_t)BtnId::Pwr);
  button_attach(&pwr_, BTN_PRESS_DOWN, onPressDown);
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
