#pragma once
#include <Arduino.h>

// Driver minimo do RTC PCF85063 (I2C, endereco 0x51) via Wire.
// Mapa de registradores confirmado em
// waveshareteam/ESP32-S3-ePaper-1.54 (SensorLib/src/REG/PCF85063Constants.h).
// So o necessario para carimbar hora nos arquivos de nota - nao usa alarmes
// nem o pino de interrupcao.
struct RtcDateTime {
  uint16_t year;  // ano completo, ex: 2026
  uint8_t month;  // 1-12
  uint8_t day;    // 1-31
  uint8_t hour;   // 0-23
  uint8_t minute; // 0-59
  uint8_t second; // 0-59
};

class Rtc {
public:
  bool begin(uint8_t sdaPin, uint8_t sclPin, uint8_t i2cAddr = 0x51);

  bool setDateTime(const RtcDateTime &dt);
  bool getDateTime(RtcDateTime &out);

  // "YYYYMMDD-HHMMSS" para nomes de arquivo - buf deve ter >= 16 bytes.
  static void formatForFilename(const RtcDateTime &dt, char *buf, size_t bufLen);

private:
  uint8_t addr_ = 0x51;
  bool ok_ = false;

  bool writeReg(uint8_t reg, const uint8_t *data, uint8_t len);
  bool readReg(uint8_t reg, uint8_t *data, uint8_t len);
  static uint8_t toBcd(uint8_t value);
  static uint8_t fromBcd(uint8_t value);
};
