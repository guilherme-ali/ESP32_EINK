#pragma once
#include "progress_model.h"
#include "../net/settings.h"
#include "../net/stt.h"
#include "../net/gdrive.h"
#include "../storage/notes.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Controlador de um unico lote. start/acknowledgeFinished sao exclusivos do
// loop principal; o worker possui configuracao, FS, clientes TLS e NVS no lote.
class SyncJob {
public:
  static constexpr unsigned kMaxNotes = 128;
  enum class StartResult { Started, NothingPending, NoMemory, TaskError };
  struct Snapshot {
    SyncView::NoteProgress progress;
    SyncTelemetry::Counters counters;
    char noteLabel[24] = "";
    char detail[72] = "";
    char transcriptModel[96] = "";
    char markdownModel[96] = "";
    char error[512] = "";
    char warning[96] = "";
    SyncTelemetry::Phase phase = SyncTelemetry::Phase::Preparing;
    uint16_t currentNote = 0, completedNotes = 0, processedNotes = 0, totalNotes = 0;
    uint16_t ai = 0, uploaded = 0, failed = 0;
    uint32_t startedAt = 0;
    bool delivered = false; // somente depois da verificacao final da nota
    bool finished = false;  // inclui limpeza Files, TLS e liberacao do contexto
    bool pauseRequested = false, paused = false;
  };

  SyncJob(SettingsStore &settings, SttClient &stt, GDriveClient &drive);
  StartResult start(NotesStore &notes, int onlyIndex = -1, const char *onlyPath = nullptr);
  bool isActive() const;
  bool snapshot(Snapshot &out) const;
  void requestPause();
  bool acknowledgeFinished();
  static const char *phaseLabel(SyncTelemetry::Phase phase);

private:
  struct Context;
  SettingsStore &settings_;
  SttClient &stt_;
  GDriveClient &drive_;
  Context *context_ = nullptr;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  Snapshot shared_, work_;
  uint32_t revision_ = 0;
  bool active_ = false, pause_ = false;
  static SyncJob *observerOwner_;
  static void taskEntry(void *arg);
  static void observe(const SyncTelemetry::Update &event);
  void run();
  void publish();
  bool pauseRequested() const;
  bool boundary();
  bool prepareAi(unsigned index);
  bool processNote(unsigned index);
  void updateDriveTotals(unsigned index);
  void saveReport(unsigned index, bool success, uint32_t startedAt);
};
