#include "../../src/sync/progress_model.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <type_traits>

namespace {
using SyncTelemetry::Phase;
using SyncView::NoteProgress;
using SyncView::Prediction;
struct Test { const char *name; std::function<void()> run; };
std::vector<Test> &tests() { static std::vector<Test> list; return list; }
struct Register { Register(const char *name, std::function<void()> run) { tests().push_back({name, run}); } };
#define TEST(name) void name(); Register reg_##name(#name, name); void name()
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string("linha ") + std::to_string(__LINE__) + ": " + #c); } while (false)
Prediction split80() { return {5, 40, 5, 5, 40, 5}; }

TEST(default_prediction_and_stack_contract) {
  Prediction p;
  CHECK(p.preparingMs == 3000 && p.geminiUploadMs == 2000 && p.transcribeMs == 10000);
  CHECK(p.markdownMs == 8000 && p.driveUploadMs == 2000 && p.verifyingMs == 1000);
  static_assert(sizeof(NoteProgress) <= 192, "modelo deve caber na pilha do worker");
  static_assert(std::is_trivially_copyable<NoteProgress>::value, "snapshot sem ponteiros de UI");
  NoteProgress m; CHECK(m.percent(9) == 0 && m.elapsedMs(9) == 0 && !m.overdue(9));
}
TEST(upload_weight_80_split_destinations_and_phase_order) {
  NoteProgress m; m.begin(split80(), 0, false, false, true);
  CHECK(m.percent(0) == 0);
  m.update(Phase::GeminiUpload, 40, 80, 0); // dados base64 inline
  CHECK(m.percent(0) == 25 && !m.isEstimate());
  m.update(Phase::Transcribing, 0, 0, 0);
  CHECK(m.percent(0) == 45 && m.isEstimate()); // upload Gemini fechado
  m.transcriptDone(); CHECK(m.percent(0) == 50);
  m.update(Phase::Markdown, 0, 0, 0); m.markdownDone(); CHECK(m.percent(0) == 55);
  uint64_t sizes[3] = {80, 10, 10}, confirmed[3] = {};
  m.setDriveTotals(sizes, confirmed);
  m.update(Phase::DriveUpload, 50, 80, 0, "note.wav");
  CHECK(m.percent(0) == 75 && !m.isEstimate());
  m.driveDone(); CHECK(m.percent(0) == 95);
  m.update(Phase::Verifying, 0, 0, 0); CHECK(m.percent(999) == 99);
  m.update(Phase::Done, 0, 0, 999); CHECK(m.percent(99999) == 99);
  m.verified(); CHECK(m.percent(99999) == 100);
}
TEST(drive_per_file_bytes_never_add_retries_or_literal_events) {
  NoteProgress m; m.begin({0, 0, 0, 0, 100, 0}, 0, true, true, true);
  uint64_t sizes[3] = {80, 10, 10}, done[3] = {};
  m.setDriveTotals(sizes, done);
  m.update(Phase::DriveUpload, 40, 80, 0, "/notes/a.wav"); CHECK(m.percent(0) == 40);
  m.update(Phase::DriveUpload, 5, 80, 1, "a.wav"); CHECK(m.percent(1) == 40);
  m.update(Phase::DriveUpload, 999, 999, 2, "retentativa multipart"); CHECK(m.percent(2) == 40);
  m.update(Phase::DriveUpload, 5, 10, 3, "a.txt"); CHECK(m.percent(3) == 45);
  m.update(Phase::DriveUpload, 5, 10, 4, "a.txt"); CHECK(m.percent(4) == 45);
  m.update(Phase::DriveUpload, 5, 10, 5, "a.md"); CHECK(m.percent(5) == 50);
  m.update(Phase::DriveUpload, 80, 80, 6, "a.wav"); CHECK(m.percent(6) == 90);
  m.update(Phase::DriveUpload, 10, 10, 7, "a.txt"); CHECK(m.percent(7) == 95);
  m.update(Phase::DriveUpload, 10, 10, 8, "a.md"); CHECK(m.percent(8) == 99);
}
TEST(confirmed_initial_totals_and_retry_snapshot_high_water) {
  NoteProgress m; m.begin({0, 0, 0, 0, 100, 0}, 0, true, true, true);
  uint64_t sizes[3] = {80, 10, 10}, done[3] = {40, 10, 0}; m.setDriveTotals(sizes, done);
  CHECK(m.percent(0) == 50);
  done[0] = 5; done[1] = 0; m.setDriveTotals(sizes, done); CHECK(m.percent(0) == 50);
  m.update(Phase::DriveUpload, 60, 80, 0, "a.wav"); CHECK(m.percent(0) == 70);
}
TEST(unknown_extensions_and_null_detail_never_credit_bytes) {
  NoteProgress m; m.begin({0, 0, 0, 0, 100, 0}, 0, true, true, true);
  uint64_t sizes[3] = {100, 0, 0}, done[3] = {}; m.setDriveTotals(sizes, done);
  for (const char *name : {"a.wav.tmp", "a.w", "a.", "a", "a.wav/d", "a.md more", static_cast<const char *>(nullptr)}) {
    m.update(Phase::DriveUpload, 100, 100, 0, name); CHECK(m.percent(0) == 0);
  }
  char detail[] = "a.wav"; m.update(Phase::DriveUpload, 25, 100, 0, detail);
  detail[2] = 'x'; CHECK(m.percent(0) == 25); // detail copiado semanticamente, nao retido
}
TEST(huge_sizes_do_not_overflow_and_bounds_are_clamped) {
  NoteProgress m; m.begin({0, 0, 0, 0, 100, 0}, 0, true, true, true);
  uint64_t sizes[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
  uint64_t done[3] = {UINT64_MAX, UINT64_MAX / 2, 0}; m.setDriveTotals(sizes, done);
  CHECK(m.percent(0) == 50);
  sizes[0] = sizes[1] = sizes[2] = 1; m.setDriveTotals(sizes, done); CHECK(m.percent(0) == 66);
  m.update(Phase::DriveUpload, UINT64_MAX, 1, 0, "a.md"); CHECK(m.percent(0) == 99);
}
TEST(estimated_stage_cap_95_and_noncompletion) {
  for (Phase p : {Phase::Preparing, Phase::Transcribing, Phase::Markdown, Phase::Verifying}) {
    Prediction prediction = {0, 0, 0, 0, 0, 0};
    switch (p) {
      case Phase::Preparing: prediction.preparingMs = 100; break;
      case Phase::Transcribing: prediction.transcribeMs = 100; break;
      case Phase::Markdown: prediction.markdownMs = 100; break;
      default: prediction.verifyingMs = 100; break;
    }
    NoteProgress m; m.begin(prediction, 0, false, false, true); m.update(p, 0, 0, 0);
    CHECK(m.percent(50) == 50 && m.percent(100) == 95 && m.percent(10000) == 95);
    CHECK(m.overdue(10000) && m.remainingMs(10000) == 0);
  }
}
TEST(waits_and_errors_freeze_progress_but_elapsed_and_eta_continue) {
  for (Phase wait : {Phase::QuotaWait, Phase::RetryWait, Phase::Error, Phase::Done}) {
    NoteProgress m; m.begin({0, 0, 100, 0, 0, 0}, 0, false, false, true);
    m.update(Phase::Transcribing, 0, 0, 0); m.update(wait, 0, 0, 40);
    CHECK(m.percent(60) == 40 && m.elapsedMs(60) == 60 && m.remainingMs(60) == 40);
    CHECK(m.percent(1000) == 40 && m.overdue(1000));
    m.update(Phase::Transcribing, 0, 0, 1000); CHECK(m.percent(1010) == 50);
  }
}
TEST(retry_upload_offsets_are_monotonic_and_time_does_not_invent_bytes) {
  NoteProgress m; m.begin({0, 100, 0, 0, 0, 0}, 0, false, false, true);
  m.update(Phase::GeminiUpload, 40, 100, 0); CHECK(m.percent(500) == 40);
  m.update(Phase::RetryWait, 0, 0, 500); CHECK(m.percent(900) == 40);
  m.update(Phase::GeminiUpload, 5, 100, 900); CHECK(m.percent(1000) == 40);
  m.update(Phase::GeminiUpload, 60, 100, 1000); CHECK(m.percent(1000) == 60);
}
TEST(upload_literal_phase_after_wait_never_invents_measured_bytes) {
  NoteProgress m; m.begin({0, 100, 0, 0, 0, 0}, 0, false, false, true);
  m.update(Phase::GeminiUpload, 40, 100, 0); m.update(Phase::RetryWait, 0, 0, 50);
  m.update(Phase::GeminiUpload, 0, 0, 5000); CHECK(m.percent(10000) == 40 && !m.isEstimate());
  m.update(Phase::GeminiUpload, 10, 100, 10000); CHECK(m.percent(20000) == 40);
}
TEST(reused_transcript_markdown_and_optional_drive) {
  NoteProgress m; m.begin(split80(), 0, true, false, true); CHECK(m.percent(0) == 45);
  m.begin(split80(), 0, true, true, true); CHECK(m.percent(0) == 50);
  m.begin(split80(), 0, true, true, false); CHECK(m.percent(0) == 95);
  CHECK(m.remainingMs(0) == 5);
  m.update(Phase::Verifying, 0, 0, 0); CHECK(m.percent(100) == 99);
}
TEST(drive_ready_verify_only_frame_and_delivery_gate) {
  NoteProgress m; m.begin(Prediction(), 7, true, true, true, true);
  CHECK(m.percent(7) == 99 && m.remainingMs(7) == 1000);
  m.update(Phase::Verifying, 0, 0, 7); CHECK(m.percent(10007) == 99 && m.overdue(10007));
  m.update(Phase::Done, 0, 0, 10007); CHECK(m.percent(10007) == 99);
  m.verified(); CHECK(m.percent(10007) == 100 && !m.overdue(10007) && !m.isEstimate());
}
TEST(millis_wrap_eta_and_restart_clear_state) {
  NoteProgress m; m.begin({100, 0, 0, 0, 0, 0}, UINT32_MAX - 9, false, false, true);
  CHECK(m.elapsedMs(10) == 20 && m.remainingMs(10) == 80 && m.percent(10) == 20);
  m.update(Phase::RetryWait, 0, 0, 10); CHECK(m.percent(1000) == 20 && m.overdue(1000));
  m.verified(); m.begin(split80(), 10, false, false, true);
  CHECK(m.percent(10) == 0 && m.remainingMs(10) == 100);
}
TEST(zero_and_maximum_prediction_are_bounded) {
  NoteProgress m; m.begin({0, 0, 0, 0, 0, 0}, 0, false, false, true);
  CHECK(m.percent(0) == 0 && m.overdue(0)); m.verified(); CHECK(m.percent(0) == 100);
  m.begin({UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}, 0, false, false, true);
  CHECK(m.remainingMs(0) == UINT32_MAX && m.percent(0) == 0);
}
TEST(note_one_of_four_is_separate_controller_counter) {
  unsigned completedNotes = 0; constexpr unsigned totalNotes = 4;
  NoteProgress note; note.begin(split80(), 0, false, false, true); note.verified(); ++completedNotes;
  CHECK(note.percent(0) == 100 && completedNotes == 1 && totalNotes == 4);
  note.begin(split80(), 0, false, false, true);
  CHECK(note.percent(0) == 0 && completedNotes == 1);
}
}
int main() {
  size_t failed = 0;
  for (const auto &test : tests()) {
    try { test.run(); }
    catch (const std::exception &e) { ++failed; std::cout << "FAIL " << test.name << ": " << e.what() << '\n'; }
  }
  std::cout << "Portable progress tests: " << tests().size() - failed << '/' << tests().size() << " OK\n";
  return failed ? 1 : 0;
}
