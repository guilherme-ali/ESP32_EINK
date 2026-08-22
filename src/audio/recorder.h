#pragma once
#include <Arduino.h>
#include <FS.h>
#include "codec.h"

// Grava PCM 16-bit mono a partir do ES8311 (que fala I2S estereo - so o
// canal esquerdo e mantido) direto num arquivo WAV em LittleFS.
class Recorder {
public:
  bool start(const char *path, uint32_t sampleRate);
  // Le um bloco do codec e acrescenta ao arquivo. Retorna false em erro
  // de I2S ou de escrita (chamador deve chamar stop() e descartar).
  bool feed(AudioCodec &codec);
  // Fecha o arquivo corrigindo o cabecalho WAV com o tamanho final.
  // Retorna o numero de bytes de audio (sem contar o cabecalho).
  uint32_t stop();

  uint32_t bytesWritten() const { return dataBytes_; }

private:
  File file_;
  uint32_t sampleRate_ = 8000;
  uint32_t dataBytes_ = 0;
  bool active_ = false;
};
