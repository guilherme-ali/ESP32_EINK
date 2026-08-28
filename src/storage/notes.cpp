#include "notes.h"
#include "../audio/wav.h"
#include <LittleFS.h>
#include <string.h>

namespace {
constexpr const char *kNotesDir = "/notes";
constexpr int kMaxNotes = 128;

NoteEntry g_cache[kMaxNotes];
int g_cacheCount = 0;
bool g_dirty = true; // forca a primeira varredura

// Escaneia /notes (poucas dezenas de arquivos, cabe no orcamento de
// flash de audio sem compressao) e ordena do mais novo para o mais
// antigo - o nome do arquivo e o timestamp, entao ordenar a string
// basta. So roda quando g_dirty (ver rescanIfDirty()) - navegar o menu
// nao mexe em /notes, entao nao precisa reler o diretorio a cada tecla.
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
        e.hasTxt = false;
        e.hasMd = false;
        e.hasSnc = false;

        WavHeader hdr;
        e.sampleRateHz = (f.size() >= sizeof(hdr) && f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr))
                             ? hdr.sampleRate
                             : 0;

        g_cacheCount++;
      }
    }
    f = dir.openNextFile();
  }

  // Segunda passagem pela mesma pasta - so metadados de diretorio (sem
  // abrir/ler conteudo), pra marcar quais .wav ja tem .txt/.snc do
  // lado. Substitui um LittleFS.exists() por nota (medido em ~13ms
  // cada nesta flash) por comparacoes de string em memoria.
  File dir2 = LittleFS.open(kNotesDir);
  if (dir2 && dir2.isDirectory()) {
    File f2 = dir2.openNextFile();
    while (f2) {
      if (!f2.isDirectory()) {
        String name = f2.name();
        int slash = name.lastIndexOf('/');
        String base = slash >= 0 ? name.substring(slash + 1) : name;
        bool isTxt = base.endsWith(".txt");
        bool isMd = base.endsWith(".md");
        bool isSnc = base.endsWith(".snc");
        if (isTxt || isMd || isSnc) {
          int dot = base.lastIndexOf('.');
          String label = dot >= 0 ? base.substring(0, dot) : base;
          for (int i = 0; i < g_cacheCount; i++) {
            if (label == g_cache[i].label) {
              if (isTxt) g_cache[i].hasTxt = true;
              else if (isMd) g_cache[i].hasMd = true;
              else g_cache[i].hasSnc = true;
              break;
            }
          }
        }
      }
      f2 = dir2.openNextFile();
    }
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

void rescanIfDirty() {
  if (!g_dirty) return;
  rescan();
  g_dirty = false;
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
  rescanIfDirty();
  return g_cacheCount;
}

bool NotesStore::getAt(int index, NoteEntry &out) {
  if (index < 0 || index >= g_cacheCount) return false;
  out = g_cache[index];
  return true;
}

int NotesStore::countPendingSync() {
  rescanIfDirty();
  int pending = 0;
  for (int i = 0; i < g_cacheCount; i++) {
    if (!g_cache[i].hasSnc) pending++;
  }
  return pending;
}

bool NotesStore::deleteAt(int index) {
  rescanIfDirty();
  if (index < 0 || index >= g_cacheCount) return false;

  String wavPath = String(g_cache[index].path);
  String txtPath = wavPath; txtPath.replace(".wav", ".txt");
  String mdPath = wavPath; mdPath.replace(".wav", ".md");
  String sncPath = wavPath; sncPath.replace(".wav", ".snc");

  bool ok = LittleFS.remove(wavPath);
  if (g_cache[index].hasTxt) LittleFS.remove(txtPath);
  if (g_cache[index].hasMd) LittleFS.remove(mdPath);
  if (g_cache[index].hasSnc) LittleFS.remove(sncPath);
  g_dirty = true;
  return ok;
}

int NotesStore::deleteAll() {
  rescanIfDirty();
  int removed = 0;
  for (int i = 0; i < g_cacheCount; i++) {
    String wavPath = String(g_cache[i].path);
    String txtPath = wavPath; txtPath.replace(".wav", ".txt");
    String mdPath = wavPath; mdPath.replace(".wav", ".md");
    String sncPath = wavPath; sncPath.replace(".wav", ".snc");
    if (LittleFS.remove(wavPath)) removed++;
    if (g_cache[i].hasTxt) LittleFS.remove(txtPath);
    if (g_cache[i].hasMd) LittleFS.remove(mdPath);
    if (g_cache[i].hasSnc) LittleFS.remove(sncPath);
  }
  g_cacheCount = 0;
  g_dirty = true;
  return removed;
}

void NotesStore::markDirty() { g_dirty = true; }

uint64_t NotesStore::totalBytes() { return LittleFS.totalBytes(); }
uint64_t NotesStore::usedBytes() { return LittleFS.usedBytes(); }
uint64_t NotesStore::freeBytes() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }
