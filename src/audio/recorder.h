#pragma once
#include <Arduino.h>
#include <FS.h>
#include "codec.h"
#include <atomic>

// Grava PCM 16-bit mono a partir do ES8311 (que fala I2S estereo - so o
// canal esquerdo e mantido) em PCM append-only e cabecalho WAV separado.
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

  // API bloqueante para chamadores sem UI. A UI usa requestStop/finish:
  // o dreno/finalizacao roda no worker, sem bloquear o loop/watchdog.
  uint32_t stop();
  void requestStop() { stopRequested_.store(true); }
  bool isFinished() const { return captureTaskDone_.load() && writeTaskDone_.load(); }
  uint32_t finish(); // somente depois de isFinished(); libera ring e marca inativo

  bool isActive() const { return active_; }
  uint32_t bytesWritten() const { return dataBytes_; }
  uint32_t maximumBytes() const { return maximumBytes_; }
  uint32_t checkpointBytes() const { return checkpointBytes_; }

  // Quantas vezes o ring buffer encheu e a task leitora teve que
  // descartar audio - deve ficar em 0 sempre; exposto para diagnostico.
  uint32_t overflowCount() const { return overflowCount_; }
  bool storageFull() const { return storageFull_; }
  bool writeFailed() const { return writeFailed_; }

private:
  File file_;
  char path_[64] = "";
  AudioCodec *codec_ = nullptr;
  uint32_t sampleRate_ = 16000;
  std::atomic<uint32_t> dataBytes_{0};
  std::atomic<uint32_t> checkpointBytes_{0};
  std::atomic<uint32_t> overflowCount_{0};
  uint32_t maximumBytes_ = 0;
  std::atomic<bool> storageFull_{false};
  std::atomic<bool> writeFailed_{false};
  bool active_ = false;

  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> captureTaskDone_{true};
  std::atomic<bool> writeTaskDone_{true};

  void *ring_ = nullptr;    // StreamBufferHandle_t (tipo escondido do .h)
  void *ringStruct_ = nullptr; // StaticStreamBuffer_t*
  uint8_t *ringStorage_ = nullptr; // PSRAM

  TaskHandle_t captureTaskHandle_ = nullptr;
  TaskHandle_t writeTaskHandle_ = nullptr;

  static void captureTaskFn(void *arg);
  static void writeTaskFn(void *arg);
};
