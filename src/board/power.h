#pragma once
#include <Arduino.h>

// Controle das tres trilhas de energia da placa.
// EPD_PWR e AUDIO_PWR sao ativas em LOW; VBAT_PWR e ativa em HIGH.
// Polaridade confirmada no board_power_bsp.cpp oficial da Waveshare.
//
// VBAT_PWR (GPIO17) NAO e so "liga o divisor pra ler a tensao da
// bateria" - e o LATCH de energia da bateria do circuito da placa. O
// exemplo oficial (07_BATT_PWR_Test/user_app.cpp) chama VBAT_POWER_ON()
// uma unica vez, logo no boot, e so chama VBAT_POWER_OFF() como acao
// explicita de "desligar o aparelho" (apertar um botao especifico).
// Com o cabo USB conectado, o VBUS sustenta o sistema e mascara esse
// pino ficando em LOW - o problema so aparece na bateria sozinha:
// desligar esse pino, mesmo que so um instante (como fazia antes ao
// redor de cada leitura de ADC), derruba a energia do aparelho inteiro
// na hora. Por isso vbatOn() e chamado uma vez no construtor e nunca
// mais desligado no uso normal (nem antes do deep sleep - o latch
// precisa continuar ligado pro aparelho conseguir acordar depois).
class BoardPower {
public:
  BoardPower(uint8_t epdPin, uint8_t audioPin, uint8_t vbatPin);

  void epdOn();
  void epdOff();
  void audioOn();
  void audioOff();

  // NAO chamar durante uso normal (nem antes de dormir) - corta a
  // energia da placa inteira quando rodando so na bateria. So existe
  // pra simetria com epdOff()/audioOff() e para um eventual "desligar
  // de vez" no futuro, que este firmware ainda nao implementa.
  void vbatOn();
  void vbatOff();

private:
  const uint8_t epdPin_;
  const uint8_t audioPin_;
  const uint8_t vbatPin_;
};
