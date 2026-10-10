#include "notes.h"
#include "../audio/wav.h"
#include <LittleFS.h>
#include <string.h>
#include "note_files.h"
#include "../net/gdrive.h"

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
  File dir = NoteFiles::fs().open(kNotesDir);
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
  File dir2 = NoteFiles::fs().open(kNotesDir);
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
               if (isTxt) g_cache[i].hasTxt = f2.size() > 0;
               else if (isMd) g_cache[i].hasMd = f2.size() > 0;
              else g_cache[i].hasSnc = true;
              break;
            }
          }
        }
      }
      f2 = dir2.openNextFile();
    }
  }

  for (int i = 0; i < g_cacheCount; ++i) {
    File audio = NoteFiles::openRead(g_cache[i].path);
    WavHeader header;
    g_cache[i].sizeBytes = audio ? audio.size() : 0;
    g_cache[i].sampleRateHz = audio && audio.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
                             validWavHeader(header, audio.size()) ? header.sampleRate : 0;
    audio.close();
    String txt(g_cache[i].path); txt.replace(".wav", ".txt");
    String md(g_cache[i].path); md.replace(".wav", ".md");
    NoteFiles::recoverText(txt); NoteFiles::recoverText(md);
    g_cache[i].hasTxt = NoteFiles::validText(txt);
    g_cache[i].hasMd = NoteFiles::validText(md);
    g_cache[i].hasSnc = !GDriveClient::needsUpload(g_cache[i].path, false);
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
  if (!NoteFiles::fs().exists(kNotesDir)) {
    return NoteFiles::fs().mkdir(kNotesDir);
  }
  // Exclusao de PCM pode ter sido interrompida entre rename/removes.
  // Reabrir a pasta apos cada operacao evita invalidar o iterador ativo.
  for (int cleaned = 0; cleaned < kMaxNotes; ++cleaned) {
    String pending;
    File dir = NoteFiles::fs().open(kNotesDir);
    File file = dir.openNextFile();
    while (file) {
      String name = file.name();
      if (!file.isDirectory() && name.endsWith(".wav.delete")) {
        int slash = name.lastIndexOf('/');
        pending = String(kNotesDir) + "/" + name.substring(slash + 1, name.length() - 7);
        file.close(); break;
      }
      file = dir.openNextFile();
    }
    dir.close();
    if (pending.isEmpty()) break;
    if (!NoteFiles::removeWav(pending.c_str())) return false;
  }
  g_dirty = true;
  return true;
}

void NotesStore::buildPath(const RtcDateTime &now, char *outPath, size_t outLen) {
  char stamp[16];
  Rtc::formatForFilename(now, stamp, sizeof(stamp));
  snprintf(outPath, outLen, "%s/%s.wav", kNotesDir, stamp);
  for (unsigned suffix = 1; NoteFiles::noteExists(outPath) && suffix < 10000; ++suffix) {
    snprintf(outPath, outLen, "%s/%s-%u.wav", kNotesDir, stamp, suffix);
  }
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

int NotesStore::countPendingSync(bool sttConfigured, bool driveConfigured) {
  rescanIfDirty();
  if (!sttConfigured && !driveConfigured) return 0;

  int pending = 0;
  for (int i = 0; i < g_cacheCount; i++) {
    if (!g_cache[i].sampleRateHz) continue; // sem PCM valido nao e pendencia normal
    bool needsAi = sttConfigured && (!g_cache[i].hasTxt || !g_cache[i].hasMd);
    bool needsDrive = driveConfigured && (!g_cache[i].hasSnc ||
                       (sttConfigured && (!g_cache[i].hasTxt || !g_cache[i].hasMd)));
    if (needsAi || needsDrive) pending++;
  }
  return pending;
}

int NotesStore::countPendingSync() {
  return countPendingSync(true, true);
}

bool NotesStore::deleteAt(int index) {
  rescanIfDirty();
  if (index < 0 || index >= g_cacheCount) return false;

  String wavPath = String(g_cache[index].path);

  bool ok = NoteFiles::removeWav(wavPath.c_str());
  if (!ok) return false;
  g_dirty = true;
  return ok;
}

int NotesStore::deleteAll() {
  rescanIfDirty();
  int removed = 0;
  for (int i = 0; i < g_cacheCount; i++) {
    String wavPath = String(g_cache[i].path);
    if (!NoteFiles::removeWav(wavPath.c_str())) continue;
    removed++;
  }
  g_cacheCount = 0;
  g_dirty = true;
  return removed;
}

void NotesStore::markDirty() { g_dirty = true; }

uint64_t NotesStore::totalBytes() { return NoteFiles::totalBytes(); }
uint64_t NotesStore::usedBytes() { return NoteFiles::usedBytes(); }
uint64_t NotesStore::freeBytes() { return NoteFiles::freeBytes(); }
