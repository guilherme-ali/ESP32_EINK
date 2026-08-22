#pragma once
#include <Arduino.h>
#include "codec.h"

// Toca um WAV PCM 16-bit mono de LittleFS pelo ES8311 (duplicando cada
// amostra para os dois canais do barramento I2S, que e fisicamente
// estereo). Chamada bloqueante - usada nos testes da Fase 2; a UI real
// (Fase 3+) deve chamar em pedacos a partir do loop principal.
class Player {
public:
  bool play(AudioCodec &codec, const char *path);
};
