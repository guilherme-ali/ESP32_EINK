#pragma once
#include <Arduino.h>

// Controle das tres trilhas de energia da placa.
// EPD_PWR e AUDIO_PWR sao ativas em LOW; VBAT_PWR (leitura do divisor de
// tensao da bateria) e ativa em HIGH. Polaridade confirmada no
// board_power_bsp.cpp oficial da Waveshare.
class BoardPower {
public:
  BoardPower(uint8_t epdPin, uint8_t audioPin, uint8_t vbatPin);

  void epdOn();
  void epdOff();
  void audioOn();
  void audioOff();
  void vbatOn();
  void vbatOff();

private:
  const uint8_t epdPin_;
  const uint8_t audioPin_;
  const uint8_t vbatPin_;
};
