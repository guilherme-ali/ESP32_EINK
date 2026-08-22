#include "notes.h"
#include <LittleFS.h>
#include <string.h>

namespace {
constexpr const char *kNotesDir = "/notes";
constexpr int kMaxNotes = 128;

NoteEntry g_cache[kMaxNotes];
int g_cacheCount = 0;

// Escaneia /notes toda vez (poucas dezenas de arquivos, cabe no
// orcamento de flash de audio sem compressao) e ordena do mais novo
// para o mais antigo - o nome do arquivo e o timestamp, entao ordenar
// a string basta.
void rescan() {
  g_cacheCount = 0;
  File dir = LittleFS.open(kNotesDir);
  if (!dir || !dir.isDirectory()) return;

  File f = dir.openNextFile();
  while (f && g_cacheCount < kMaxNotes) {
    if (!f.isDirectory()) {
      String name = f.name(); // pode vir com ou sem o prefixo do dir
      int slash = name.lastIndexOf('/');
      String base = slash >= 0 ? name.substring(slash + 1) : name;
      if (base.endsWith(".wav")) {
        NoteEntry &e = g_cache[g_cacheCount];
        snprintf(e.path, sizeof(e.path), "%s/%s", kNotesDir, base.c_str());
        strncpy(e.label, base.c_str(), sizeof(e.label) - 1);
        e.label[sizeof(e.label) - 1] = '\0';
        char *dot = strchr(e.label, '.');
        if (dot) *dot = '\0';
        e.sizeBytes = f.size();
        g_cacheCount++;
      }
    }
    f = dir.openNextFile();
  }

  // insertion sort descendente por label (timestamp) - poucas dezenas
  // de itens, O(n^2) e mais que suficiente.
  for (int i = 1; i < g_cacheCount; i++) {
    NoteEntry key = g_cache[i];
    int j = i - 1;
    while (j >= 0 && strcmp(g_cache[j].label, key.label) < 0) {
      g_cache[j + 1] = g_cache[j];
      j--;
    }
    g_cache[j + 1] = key;
  }
}
} // namespace

bool NotesStore::begin() {
  if (!LittleFS.exists(kNotesDir)) {
    return LittleFS.mkdir(kNotesDir);
  }
  return true;
}

void NotesStore::buildPath(const RtcDateTime &now, char *outPath, size_t outLen) {
  char stamp[16];
  Rtc::formatForFilename(now, stamp, sizeof(stamp));
  snprintf(outPath, outLen, "%s/%s.wav", kNotesDir, stamp);
}

int NotesStore::count() {
  rescan();
  return g_cacheCount;
}

bool NotesStore::getAt(int index, NoteEntry &out) {
  if (index < 0 || index >= g_cacheCount) return false;
  out = g_cache[index];
  return true;
}

uint64_t NotesStore::totalBytes() { return LittleFS.totalBytes(); }
uint64_t NotesStore::usedBytes() { return LittleFS.usedBytes(); }
uint64_t NotesStore::freeBytes() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }
