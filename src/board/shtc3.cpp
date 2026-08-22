#include "shtc3.h"
#include <Wire.h>

namespace {
constexpr uint16_t CMD_WAKEUP = 0x3517;
constexpr uint16_t CMD_SLEEP = 0xB098;
constexpr uint16_t CMD_SOFT_RESET = 0x805D;
constexpr uint16_t CMD_MEAS_T_RH_POLLING = 0x7866; // le temp primeiro, sem clock-stretch
constexpr uint8_t CRC_POLY = 0x31; // x^8+x^5+x^4+1, polinomio 0x131 sem o bit 8
} // namespace

uint8_t Shtc3::crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ CRC_POLY) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

bool Shtc3::writeCmd(uint16_t cmd) {
  Wire.beginTransmission(addr_);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Shtc3::readBytes(uint8_t *buf, uint8_t len) {
  uint8_t got = Wire.requestFrom((int)addr_, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool Shtc3::begin(uint8_t sdaPin, uint8_t sclPin, uint8_t i2cAddr) {
  addr_ = i2cAddr;
  Wire.begin(sdaPin, sclPin);

  writeCmd(CMD_WAKEUP);
  delay(1);
  ok_ = writeCmd(CMD_SOFT_RESET);
  delay(20);
  writeCmd(CMD_SLEEP);
  return ok_;
}

bool Shtc3::read(float &outTempC, float &outHumidity) {
  if (!writeCmd(CMD_WAKEUP)) return false;
  delay(1);

  if (!writeCmd(CMD_MEAS_T_RH_POLLING)) {
    writeCmd(CMD_SLEEP);
    return false;
  }
  delay(20); // tempo de medida em modo normal (nao low-power)

  uint8_t raw[6];
  bool gotData = readBytes(raw, sizeof(raw));
  writeCmd(CMD_SLEEP);
  if (!gotData) return false;

  if (crc8(raw, 2) != raw[2] || crc8(raw + 3, 2) != raw[5]) return false;

  uint16_t rawTemp = (raw[0] << 8) | raw[1];
  uint16_t rawHumi = (raw[3] << 8) | raw[4];
  outTempC = 175.0f * rawTemp / 65536.0f - 45.0f;
  outHumidity = 100.0f * rawHumi / 65536.0f;
  return true;
}
