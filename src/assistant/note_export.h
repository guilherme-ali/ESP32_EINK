#pragma once
#include "platform.h"
#include <atomic>

// Exportação explícita de uma transcrição/resumo já persistido. O gateway cria
// o arquivo no GitHub; o plugin Obsidian Git recebe o commit no próximo pull.
class NoteExportJob {
public:
  using ConnectFn = bool (*)();
  struct Snapshot { bool finished = false, ok = false; char message[256] = {}; };
  bool start(const Settings &settings, const char *wavPath, ConnectFn connect);
  void cancel();
  bool active() const;
  bool snapshot(Snapshot &out) const;
  bool acknowledgeFinished();
private:
  struct Context;
  Context *context_ = nullptr;
  Snapshot view_;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  static void worker(void *arg);
  void reap();
};
