#include "power.h"
#include <driver/gpio.h>

BoardPower::BoardPower(uint8_t epdPin, uint8_t audioPin, uint8_t vbatPin)
    : epdPin_(epdPin), audioPin_(audioPin), vbatPin_(vbatPin) {}

void BoardPower::begin() {
  // Preload all outputs HIGH while their previous sleep holds are still set.
  // In particular, never expose the battery latch's reset LOW level.
  gpio_set_level(static_cast<gpio_num_t>(vbatPin_), 1);
  gpio_set_level(static_cast<gpio_num_t>(epdPin_), 1);
  gpio_set_level(static_cast<gpio_num_t>(audioPin_), 1);
  pinMode(epdPin_, OUTPUT);
  pinMode(audioPin_, OUTPUT);
  pinMode(vbatPin_, OUTPUT);
  epdOff();
  audioOff();
  vbatOn(); // latch de energia da bateria - ver comentario em power.h
}

void BoardPower::epdOn()    { digitalWrite(epdPin_, LOW); }
void BoardPower::epdOff()   { digitalWrite(epdPin_, HIGH); }
void BoardPower::audioOn()  { digitalWrite(audioPin_, LOW); }
void BoardPower::audioOff() { digitalWrite(audioPin_, HIGH); }
void BoardPower::vbatOn()   { digitalWrite(vbatPin_, HIGH); }
void BoardPower::vbatOff()  { digitalWrite(vbatPin_, LOW); }
