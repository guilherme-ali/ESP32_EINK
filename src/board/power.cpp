#include "power.h"

BoardPower::BoardPower(uint8_t epdPin, uint8_t audioPin, uint8_t vbatPin)
    : epdPin_(epdPin), audioPin_(audioPin), vbatPin_(vbatPin) {
  pinMode(epdPin_, OUTPUT);
  pinMode(audioPin_, OUTPUT);
  pinMode(vbatPin_, OUTPUT);
  epdOff();
  audioOff();
  vbatOff();
}

void BoardPower::epdOn()    { digitalWrite(epdPin_, LOW); }
void BoardPower::epdOff()   { digitalWrite(epdPin_, HIGH); }
void BoardPower::audioOn()  { digitalWrite(audioPin_, LOW); }
void BoardPower::audioOff() { digitalWrite(audioPin_, HIGH); }
void BoardPower::vbatOn()   { digitalWrite(vbatPin_, HIGH); }
void BoardPower::vbatOff()  { digitalWrite(vbatPin_, LOW); }
