#pragma once
#include <Arduino.h>

// Leitura da bateria via GPIO4 (ADC1_CH3), atras de um divisor
// resistivo x2 e da trilha VBAT_PWR (precisa estar ligada - ver
// board/power.h - antes de ler; o chamador controla isso porque a
// trilha e compartilhada e ligar/desligar tem custo de tempo).
class Battery {
public:
  // Volts reais na bateria (ja compensando o divisor x2).
  static float readVoltage();

  // Estimativa grosseira 0-100% pra uma LiPo 1S generica (curva nao
  // linear real; 3.30V=0%, 4.20V=100%, com uma curva mais suave no
  // meio - o objetivo e "cheio/ok/baixo", nao precisao de fuel gauge).
  static int readPercent();
};
