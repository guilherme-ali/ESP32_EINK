#include "progress_model.h"
#include <cmath>

namespace SyncView {
namespace {
constexpr unsigned kStages = 6;
unsigned stage(SyncTelemetry::Phase phase) { return static_cast<unsigned>(phase); }
float ratio(uint64_t current, uint64_t total) {
  if (!total) return 0;
  if (current >= total) return 1;
  return static_cast<float>(static_cast<double>(current) / static_cast<double>(total));
}
int driveIndex(const char *detail) {
  if (!detail) return -1;
  const char *suffix = nullptr;
  for (const char *p = detail; *p; ++p) {
    if (*p == '/' || *p == '\\') suffix = nullptr;
    if (*p == '.') suffix = p + 1;
  }
  if (!suffix) return -1;
  // Somente extensoes completas: uma fase literal nao identifica um arquivo.
  if (suffix[0] == 'm' && suffix[1] == 'd' && !suffix[2]) return 2;
  if (suffix[0] == 'w' && suffix[1] == 'a' && suffix[2] == 'v' && !suffix[3]) return 0;
  if (suffix[0] == 't' && suffix[1] == 'x' && suffix[2] == 't' && !suffix[3]) return 1;
  return -1;
}
}

void NoteProgress::begin(const Prediction &p, uint32_t now, bool transcriptReady,
                         bool markdownReady, bool driveRequired, bool driveReady) {
  *this = NoteProgress();
  started_ = true;
  startedAt_ = phaseAt_ = now;
  const uint32_t values[kStages] = {p.preparingMs, p.geminiUploadMs, p.transcribeMs,
                                   p.markdownMs, p.driveUploadMs, p.verifyingMs};
  uint64_t expected = 0;
  for (unsigned i = 0; i < kStages; ++i) durations_[i] = values[i];
  if (transcriptReady) transcriptDone();
  if (markdownReady) markdownDone();
  if (!driveRequired || driveReady) driveDone();
  if (transcriptReady && markdownReady && (!driveRequired || driveReady)) fractions_[0] = 1;
  for (unsigned i = 0; i < kStages; ++i)
    if (fractions_[i] < 1) expected += durations_[i];
  expectedMs_ = expected > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(expected);
  // Todos os artefatos ja confirmados: ainda falta a entrega final do worker.
  if (transcriptReady && markdownReady && driveReady) highWater_ = 99;
}

void NoteProgress::accrue(uint32_t now) {
  unsigned i = stage(phase_);
  if (i < kStages && !verified_) {
    uint64_t spent = static_cast<uint64_t>(spent_[i]) + (now - phaseAt_);
    spent_[i] = spent > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(spent);
    if (!measured_ && fractions_[i] < 1) {
      float value = durations_[i] ? ratio(spent_[i], durations_[i]) : 0.95f;
      if (value > 0.95f) value = 0.95f;
      if (value > fractions_[i]) fractions_[i] = value;
    }
  }
  phaseAt_ = now;
}

void NoteProgress::update(SyncTelemetry::Phase next, uint64_t current, uint64_t total,
                          uint32_t now, const char *detail) {
  if (!started_ || verified_) return;
  // Congela a fracao calculada no instante de entrada em espera/erro.
  percent(now);
  phase_ = next;
  phaseAt_ = now;
  unsigned i = stage(next);
  measured_ = next == SyncTelemetry::Phase::GeminiUpload && geminiMeasured_;
  if (i > 0 && i < kStages) fractions_[0] = 1;
  if (next == SyncTelemetry::Phase::Transcribing) fractions_[1] = 1;
  if (next == SyncTelemetry::Phase::GeminiUpload && total) {
    measured_ = geminiMeasured_ = true;
    float value = ratio(current, total); // inline: bytes base64, nao WAV cru
    if (value > fractions_[1]) fractions_[1] = value;
  }
  if (next == SyncTelemetry::Phase::DriveUpload) {
    int index = driveIndex(detail);
    if (index >= 0 && total) {
      driveSizes_[index] = total;
      uint64_t bounded = current > total ? total : current;
      if (bounded > driveConfirmed_[index]) driveConfirmed_[index] = bounded;
      if (driveConfirmed_[index] > total) driveConfirmed_[index] = total;
      driveFraction();
    }
    measured_ = driveSizes_[0] || driveSizes_[1] || driveSizes_[2];
  }
  // Esperas e erros nao recebem progresso estimado nem concluem a nota.
}

void NoteProgress::setDriveTotals(const uint64_t sizes[3], const uint64_t confirmed[3]) {
  if (!sizes || !confirmed) return;
  for (unsigned i = 0; i < 3; ++i) {
    driveSizes_[i] = sizes[i];
    uint64_t bounded = confirmed[i] > sizes[i] ? sizes[i] : confirmed[i];
    if (bounded > driveConfirmed_[i]) driveConfirmed_[i] = bounded;
    if (driveConfirmed_[i] > sizes[i]) driveConfirmed_[i] = sizes[i];
  }
  driveFraction();
  if (phase_ == SyncTelemetry::Phase::DriveUpload)
    measured_ = sizes[0] || sizes[1] || sizes[2];
}
void NoteProgress::driveFraction() {
  // double evita overflow mesmo com tres tamanhos uint64_t maximos.
  double done = 0, total = 0;
  for (unsigned i = 0; i < 3; ++i) {
    total += static_cast<double>(driveSizes_[i]);
    done += static_cast<double>(driveConfirmed_[i]);
  }
  if (total > 0) fractions_[4] = static_cast<float>(done / total);
}
void NoteProgress::transcriptDone() { fractions_[1] = fractions_[2] = 1; }
void NoteProgress::markdownDone() { fractions_[3] = 1; }
void NoteProgress::driveDone() { fractions_[4] = 1; }
void NoteProgress::verified() { verified_ = true; highWater_ = 100; }

unsigned NoteProgress::percent(uint32_t now) {
  if (!started_) return 0;
  if (verified_) return 100;
  accrue(now);
  double done = 0, total = 0;
  for (unsigned i = 0; i < kStages; ++i) {
    total += durations_[i];
    done += static_cast<double>(durations_[i]) * fractions_[i];
  }
  unsigned result = total > 0 ? static_cast<unsigned>(std::floor(100 * done / total + 0.00001)) : 0;
  if (result > 99) result = 99;
  if (result > highWater_) highWater_ = result;
  return highWater_;
}
uint32_t NoteProgress::elapsedMs(uint32_t now) const { return started_ ? now - startedAt_ : 0; }
uint32_t NoteProgress::remainingMs(uint32_t now) const {
  if (!started_ || verified_) return 0;
  uint32_t elapsed = elapsedMs(now);
  return elapsed >= expectedMs_ ? 0 : expectedMs_ - elapsed;
}
bool NoteProgress::overdue(uint32_t now) const {
  return started_ && !verified_ && elapsedMs(now) >= expectedMs_;
}
bool NoteProgress::isEstimate() const { return started_ && !verified_ && !measured_; }
} // namespace SyncView
