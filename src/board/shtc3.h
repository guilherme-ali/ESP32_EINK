#pragma once
#include <Arduino.h>

// Driver minimo do sensor SHTC3 (I2C, endereco 0x70) via Wire.
// Comandos e polinomio de CRC confirmados em
// waveshareteam/ESP32-S3-ePaper-1.54 (02_I2C_PCF85063/i2c_equipment.cpp).
// So o necessario pra mostrar temperatura/umidade na tela de bloqueio -
// sem os modos de baixa repetibilidade nem clock-stretching.
class Shtc3 {
public:
  bool begin(uint8_t sdaPin, uint8_t sclPin, uint8_t i2cAddr = 0x70);

  // true se leu com sucesso (CRC ok); false deixa outTempC/outHumidity
  // inalterados. Faz wakeup -> mede -> sleep sozinho (o sensor gasta
  // pouca energia dormindo, entao fica sempre em sleep entre leituras).
  bool read(float &outTempC, float &outHumidity);

private:
  uint8_t addr_ = 0x70;
  bool ok_ = false;

  bool writeCmd(uint16_t cmd);
  bool readBytes(uint8_t *buf, uint8_t len);
  static uint8_t crc8(const uint8_t *data, int len);
};
