#include "rtc.h"
#include <Wire.h>
#include <stdio.h>

namespace {
constexpr uint8_t REG_CTRL1 = 0x00;
constexpr uint8_t REG_SEC = 0x04;
// REG_MIN=0x05, REG_HR=0x06, REG_DAY=0x07, REG_WEEKDAY=0x08,
// REG_MONTH=0x09, REG_YEAR=0x0A - todos sequenciais apos REG_SEC.
constexpr uint8_t SEC_OS_FLAG_MASK = 0x80; // bit7 de Seconds: oscillator stop
} // namespace

uint8_t Rtc::toBcd(uint8_t value) { return ((value / 10) << 4) | (value % 10); }
uint8_t Rtc::fromBcd(uint8_t value) { return ((value >> 4) * 10) + (value & 0x0F); }

bool Rtc::writeReg(uint8_t reg, const uint8_t *data, uint8_t len) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

bool Rtc::readReg(uint8_t reg, uint8_t *data, uint8_t len) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((int)addr_, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
}

bool Rtc::begin(uint8_t sdaPin, uint8_t sclPin, uint8_t i2cAddr) {
  addr_ = i2cAddr;
  Wire.begin(sdaPin, sclPin);

  uint8_t ctrl1 = 0x00; // 24h, clock habilitado, sem test/softreset
  ok_ = writeReg(REG_CTRL1, &ctrl1, 1);
  return ok_;
}

bool Rtc::setDateTime(const RtcDateTime &dt) {
  uint8_t buf[7];
  buf[0] = toBcd(dt.second) & ~SEC_OS_FLAG_MASK; // limpa flag de oscilador parado
  buf[1] = toBcd(dt.minute);
  buf[2] = toBcd(dt.hour);
  buf[3] = toBcd(dt.day);
  buf[4] = 0; // weekday nao usado
  buf[5] = toBcd(dt.month);
  buf[6] = toBcd((uint8_t)(dt.year - 2000));
  return writeReg(REG_SEC, buf, sizeof(buf));
}

bool Rtc::getDateTime(RtcDateTime &out) {
  uint8_t buf[7];
  if (!readReg(REG_SEC, buf, sizeof(buf))) return false;

  out.second = fromBcd(buf[0] & ~SEC_OS_FLAG_MASK);
  out.minute = fromBcd(buf[1] & 0x7F);
  out.hour = fromBcd(buf[2] & 0x3F);
  out.day = fromBcd(buf[3] & 0x3F);
  out.month = fromBcd(buf[5] & 0x1F);
  out.year = 2000 + fromBcd(buf[6]);
  return true;
}

void Rtc::formatForFilename(const RtcDateTime &dt, char *buf, size_t bufLen) {
  snprintf(buf, bufLen, "%04u%02u%02u-%02u%02u%02u",
            dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}
