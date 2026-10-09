#include "note_files.h"
#include "../net/gemini_client.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include "../audio/wav.h"

namespace NoteFiles {
fs::FS &fs() { return LittleFS; }
uint64_t totalBytes() { return LittleFS.totalBytes(); }
uint64_t usedBytes() { return LittleFS.usedBytes(); }
uint64_t freeBytes() { return totalBytes() - usedBytes(); }
uint64_t recordingBytes() {
  uint64_t available = freeBytes();
  return available > kReserveBytes ? available - kReserveBytes : 0;
}

bool readText(const String &path, char *out, size_t capacity) {
  if (!out || capacity < 2) return false;
  out[0] = '\0';
  File f = fs().open(path, FILE_READ);
  if (!f || f.isDirectory() || !f.size() || f.size() >= capacity ||
      f.size() > kMaxTextBytes) return false;
  size_t size = f.size();
  size_t read = f.readBytes(out, size);
  f.close();
  out[read] = '\0';
  if (read != size || !SttNet::validUtf8(out, size) || !SttNet::hasText(out)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

bool validText(const char *path) {
  File f = fs().open(path, FILE_READ);
  if (!f || !f.size() || f.size() > kMaxTextBytes) return false;
  size_t capacity = f.size() + 1;
  f.close();
  char *buffer = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) buffer = static_cast<char *>(malloc(capacity));
  if (!buffer) return false;
  bool ok = readText(path, buffer, capacity);
  free(buffer);
  return ok;
}
bool validText(const String &path) { return validText(path.c_str()); }
bool validWav(const char *path) {
  File file = fs().open(path, FILE_READ);
  WavHeader header;
  return file && file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
         validWavHeader(header, file.size());
}

bool writeAtomic(const String &path, const char *data, size_t length) {
  if (!data || !length || length > kMaxTextBytes ||
      !SttNet::validUtf8(data, length) || !SttNet::hasText(data) ||
      freeBytes() < length + 8192) return false;
  String temporary = path + ".tmp";
  File f = fs().open(temporary, FILE_WRITE);
  if (!f) return false;
  size_t written = f.write(reinterpret_cast<const uint8_t *>(data), length);
  f.flush();
  f.close();
  if (written != length || !validText(temporary)) {
    fs().remove(temporary);
    return false;
  }
  // LittleFS rename substitui o destino atomicamente; nao remover antes.
  return fs().rename(temporary, path);
}

void recoverText(const String &path) {
  String temporary = path + ".tmp";
  if (!fs().exists(temporary)) return;
  if (validText(path)) {
    fs().remove(temporary);
  } else if (validText(temporary)) {
    fs().rename(temporary, path);
  } else {
    fs().remove(temporary);
  }
}
}
