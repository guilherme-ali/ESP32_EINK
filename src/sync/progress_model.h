#pragma once
#include "telemetry.h"

namespace SyncView {
struct Prediction {
  uint32_t preparingMs = 3000;
  uint32_t geminiUploadMs = 2000;
  uint32_t transcribeMs = 10000;
  uint32_t markdownMs = 8000;
  uint32_t driveUploadMs = 2000;
  uint32_t verifyingMs = 1000;
};

// Modelo por nota; o contador de notas do lote pertence ao controlador.
// Sem alocacao, referencias ao detail, relogio global ou dependencia do SDK.
class NoteProgress {
public:
  void begin(const Prediction &prediction, uint32_t now, bool transcriptReady,
             bool markdownReady, bool driveRequired, bool driveReady = false);
  void update(SyncTelemetry::Phase phase, uint64_t current, uint64_t total,
              uint32_t now, const char *detail = "");
  // Ordem WAV/TXT/MD. Offset maximo por arquivo: retentativas nao somam bytes.
  // Informar todos os tamanhos antes do primeiro upload estabiliza o denominador.
  void setDriveTotals(const uint64_t sizes[3], const uint64_t confirmed[3]);
  void transcriptDone();
  void markdownDone();
  void driveDone();
  void verified();
  unsigned percent(uint32_t now);
  uint32_t elapsedMs(uint32_t now) const;
  // ETA nominal em tempo de parede, incluindo esperas. Ao vencer, exibir
  // overdue em vez de interpretar zero restante como prova de conclusao.
  uint32_t remainingMs(uint32_t now) const;
  bool overdue(uint32_t now) const;
  bool isEstimate() const;

private:
  uint32_t durations_[6] = {};
  uint32_t spent_[6] = {};
  float fractions_[6] = {};
  uint64_t driveSizes_[3] = {};
  uint64_t driveConfirmed_[3] = {};
  uint32_t startedAt_ = 0, phaseAt_ = 0, expectedMs_ = 0;
  SyncTelemetry::Phase phase_ = SyncTelemetry::Phase::Preparing;
  unsigned highWater_ = 0;
  bool started_ = false, verified_ = false, measured_ = false, geminiMeasured_ = false;
  void accrue(uint32_t now);
  void driveFraction();
};
} // namespace SyncView
