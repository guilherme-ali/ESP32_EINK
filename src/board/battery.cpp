#include "battery.h"
#include "../config/pins.h"

float Battery::readVoltage() {
  // Media de algumas leituras - ADC do ESP32 e ruidoso, sobretudo com
  // WiFi/BT ativos.
  uint32_t sumMv = 0;
  const int kSamples = 8;
  for (int i = 0; i < kSamples; i++) {
    sumMv += analogReadMilliVolts(PIN_VBAT_ADC);
  }
  float mv = sumMv / (float)kSamples;
  return (mv * 2.0f) / 1000.0f; // divisor resistivo x2, mV -> V
}

int Battery::readPercent() {
  float v = readVoltage();

  // Curva de descarga tipica de LiPo 1S, interpolada por trechos.
  struct Point { float volts; int percent; };
  static const Point kCurve[] = {
      {3.30f, 0}, {3.40f, 10}, {3.55f, 25}, {3.70f, 50},
      {3.85f, 70}, {4.00f, 85}, {4.20f, 100},
  };
  constexpr int kN = sizeof(kCurve) / sizeof(kCurve[0]);

  if (v <= kCurve[0].volts) return 0;
  if (v >= kCurve[kN - 1].volts) return 100;

  for (int i = 1; i < kN; i++) {
    if (v <= kCurve[i].volts) {
      const Point &a = kCurve[i - 1];
      const Point &b = kCurve[i];
      float t = (v - a.volts) / (b.volts - a.volts);
      return a.percent + (int)(t * (b.percent - a.percent));
    }
  }
  return 100;
}
