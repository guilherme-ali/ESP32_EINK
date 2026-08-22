#pragma once
#include <Arduino.h>
#include <FS.h>
#include "codec.h"

// Grava PCM 16-bit mono a partir do ES8311 (que fala I2S estereo - so o
// canal esquerdo e mantido) direto num arquivo WAV em LittleFS.
//
// A leitura do I2S e a escrita em flash rodam em duas tasks proprias no
// core 0 (o loop() do Arduino roda no core 1), ligadas por um ring
// buffer alocado em PSRAM. Isso existe por um motivo concreto: antes,
// feed() era chamado direto do loop() e competia com o refresh do
// e-paper (que bloqueia 300-500ms esperando o pino BUSY) - o buffer DMA
// do I2S so segura ~192ms nesse meio tempo, entao ~200ms de audio eram
// perdidos por segundo. Com a captura isolada numa task de prioridade
// alta, o desenho da tela nunca mais atrasa a leitura do microfone.
class Recorder {
public:
  // codec deve continuar valido (e com enable(true) + sampleRate
  // configurados pelo chamador) durante toda a gravacao.
  bool start(const char *path, uint32_t sampleRate, AudioCodec *codec);

  // Sinaliza parada, espera as duas tasks terminarem, corrige o
  // cabecalho WAV com o tamanho final e fecha o arquivo. Bloqueia por
  // no maximo alguns ms (so o tempo de esvaziar o que ja estava no ar).
  uint32_t stop();

  bool isActive() const { return active_; }
  uint32_t bytesWritten() const { return dataBytes_; }

  // Quantas vezes o ring buffer encheu e a task leitora teve que
  // descartar audio - deve ficar em 0 sempre; exposto para diagnostico.
  uint32_t overflowCount() const { return overflowCount_; }

private:
  File file_;
  AudioCodec *codec_ = nullptr;
  uint32_t sampleRate_ = 16000;
  volatile uint32_t dataBytes_ = 0;
  volatile uint32_t overflowCount_ = 0;
  bool active_ = false;

  volatile bool stopRequested_ = false;
  volatile bool captureTaskDone_ = true;
  volatile bool writeTaskDone_ = true;

  void *ring_ = nullptr;    // StreamBufferHandle_t (tipo escondido do .h)
  void *ringStruct_ = nullptr; // StaticStreamBuffer_t*
  uint8_t *ringStorage_ = nullptr; // PSRAM

  TaskHandle_t captureTaskHandle_ = nullptr;
  TaskHandle_t writeTaskHandle_ = nullptr;

  static void captureTaskFn(void *arg);
  static void writeTaskFn(void *arg);
};
