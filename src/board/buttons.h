#pragma once
#include <Arduino.h>
#include "multi_button.h"

// Wrapper fino sobre a lib multi_button (vendor, ver multi_button.c/h)
// para os dois botoes fisicos da placa: BOOT e PWR. button_ticks() deve
// ser chamado a cada ~5ms (feito internamente via millis() em poll()).
enum class BtnId : uint8_t { Boot = 1, Pwr = 2 };

enum class BtnAction : uint8_t {
  None,
  ShortClick,
  DoubleClick,
  LongPress,
  Released,
};

using ButtonEventFn = void (*)(BtnId id, BtnAction action);

class Buttons {
public:
  void begin(uint8_t bootPin, uint8_t pwrPin, ButtonEventFn onEvent);
  void poll(); // chamar a cada iteracao do loop()

  // Troca o callback sem reinicializar a maquina de estado dos botoes -
  // begin() faz memset() nos handles, entao chama-lo de novo corromperia
  // a lista ligada da multi_button. Usado para alternar entre a tela de
  // configuracao de Wi-Fi (so boot) e o app normal.
  void setCallback(ButtonEventFn onEvent);

private:
  Button boot_{};
  Button pwr_{};
  uint32_t lastTickMs_ = 0;
};
