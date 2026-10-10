#include "note_files.h"
#include "../net/gemini_client.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include "../audio/wav.h"
#include <FSImpl.h>
#include <new>

namespace {
constexpr const char *kNoteSiblings[] = {".pcm", ".txt", ".md", ".snc", ".sync", ".sync.tmp",
                                        ".txt.tmp", ".md.tmp", ".ai", ".ai.tmp", ".perf", ".perf.tmp"};
// Visao somente leitura de um WAV. O payload nunca e reescrito para mudar
// o tamanho RIFF: LittleFS CTZ copiaria todos os blocos apos o seek(0).
class WavReadFile final : public fs::FileImpl {
public:
  WavReadFile(File payload, const String &path, const WavHeader &header, uint32_t offset)
      : payload_(payload), path_(path), header_(header), offset_(offset) {}
  size_t write(const uint8_t *, size_t) override { return 0; }
  size_t read(uint8_t *out, size_t count) override {
    if (!payload_ || !out || position_ >= size()) return 0;
    count = std::min(count, size() - position_);
    size_t copied = 0;
    if (position_ < sizeof(header_)) {
      copied = std::min(count, sizeof(header_) - position_);
      memcpy(out, reinterpret_cast<const uint8_t *>(&header_) + position_, copied);
      position_ += copied;
    }
    if (copied < count) {
      uint32_t target = offset_ + position_ - sizeof(header_);
      if (payload_.position() != target && !payload_.seek(target)) return copied;
      size_t got = payload_.read(out + copied, count - copied);
      if (got > count - copied) return copied;
      copied += got; position_ += got;
    }
    return copied;
  }
  void flush() override {}
  bool seek(uint32_t pos, SeekMode mode) override {
    if (!payload_) return false;
    uint64_t target = pos;
    if (mode == SeekCur) target += position_;
    else if (mode == SeekEnd) target += size();
    else if (mode != SeekSet) return false;
    if (target > size()) return false;
    position_ = static_cast<uint32_t>(target); return true;
  }
  size_t position() const override { return position_; }
  size_t size() const override { return sizeof(header_) + header_.dataSize; }
  bool setBufferSize(size_t count) override { return payload_.setBufferSize(count); }
  void close() override { payload_.close(); }
  time_t getLastWrite() override { return payload_.getLastWrite(); }
  const char *path() const override { return path_.c_str(); }
  const char *name() const override {
    const char *slash = strrchr(path_.c_str(), '/'); return slash ? slash + 1 : path_.c_str();
  }
  boolean isDirectory() override { return false; }
  fs::FileImplPtr openNextFile(const char *) override { return {}; }
  boolean seekDir(long) override { return false; }
  String getNextFileName() override { return ""; }
  String getNextFileName(bool *isDir) override { if (isDir) *isDir = false; return ""; }
  void rewindDirectory() override {}
  operator bool() override { return static_cast<bool>(payload_); }
private:
  File payload_;
  String path_;
  WavHeader header_;
  uint32_t offset_ = 0, position_ = 0;
};

bool readHeader(File &file, WavHeader &header) {
  return file && !file.isDirectory() && file.size() >= sizeof(header) &&
         file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
         validPcmFormat(header);
}
} // namespace

namespace NoteFiles {
fs::FS &fs() { return LittleFS; }
uint64_t totalBytes() { return LittleFS.totalBytes(); }
uint64_t usedBytes() { return LittleFS.usedBytes(); }
uint64_t freeBytes() { return totalBytes() - usedBytes(); }
uint64_t recordingBytes() {
  uint64_t available = freeBytes();
  return available > kReserveBytes ? available - kReserveBytes : 0;
}

String pcmPath(const String &wavPath) {
  if (!wavPath.endsWith(".wav")) return "";
  return wavPath.substring(0, wavPath.length() - 4) + ".pcm";
}

bool noteExists(const String &wavPath) {
  if (!wavPath.endsWith(".wav")) return false;
  if (fs().exists(wavPath) || fs().exists(wavPath + ".tmp") || fs().exists(wavPath + ".delete")) return true;
  String base = wavPath.substring(0, wavPath.length() - 4);
  for (const char *extension : kNoteSiblings) if (fs().exists(base + extension)) return true;
  return false;
}

File openRead(const String &path) {
  File original = fs().open(path, FILE_READ);
  if (!path.endsWith(".wav")) return original;
  WavHeader header;
  if (!readHeader(original, header)) { if (original) original.seek(0); return original; }
  size_t storedSize = original.size();
  String pcm = pcmPath(path);
  File payload;
  uint32_t offset = 0;
  if (storedSize == sizeof(header) && fs().exists(pcm)) {
    if (header.dataSize > UINT32_MAX - 36 || header.dataSize % 2 || header.chunkSize != 36 + header.dataSize)
      return File();
    payload = fs().open(pcm, FILE_READ);
    if (!payload || payload.isDirectory()) return File();
    original.close();
  } else if (storedSize > sizeof(header) && header.dataSize == 0 && header.chunkSize == 36) {
    // Compatibilidade: WAV antigo interrompido com placeholder mas PCM
    // persistido. Corrige somente a visao lida, nunca copia/trunca o original.
    payload = original; offset = sizeof(header);
  } else {
    original.seek(0); return original; // WAV legado integro ou invalido: validador estrito.
  }
  size_t actual = (payload.size() - offset) & ~size_t(1);
  if (actual > UINT32_MAX - sizeof(header)) return File();
  header = makeWavHeader(header.sampleRate, 1, static_cast<uint32_t>(actual));
  auto *view = new (std::nothrow) WavReadFile(payload, path, header, offset);
  if (!view) return File();
  return File(fs::FileImplPtr(view));
}

bool writeWavHeader(const char *path, const WavHeader &header) {
  if (!path || !String(path).endsWith(".wav") || !validPcmFormat(header) ||
      header.dataSize % 2 || header.dataSize > UINT32_MAX - 36 ||
      header.chunkSize != 36 + header.dataSize) return false;
  String temporary = String(path) + ".tmp";
  File file = fs().open(temporary, FILE_WRITE);
  if (!file) return false;
  size_t written = file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  file.flush(); file.close();
  File check = fs().open(temporary, FILE_READ);
  WavHeader saved;
  bool ok = written == sizeof(header) && check.size() == sizeof(header) &&
            readHeader(check, saved) && memcmp(&saved, &header, sizeof(header)) == 0;
  check.close();
  if (!ok) { fs().remove(temporary); return false; }
  return fs().rename(temporary, path);
}

bool removeWav(const char *path) {
  if (!path || !String(path).endsWith(".wav")) return false;
  String tombstone = String(path) + ".delete";
  if (fs().exists(path)) {
    if (fs().exists(tombstone) || !fs().rename(path, tombstone)) return false;
  }
  String base = String(path).substring(0, strlen(path) - 4), temporary = String(path) + ".tmp";
  for (const char *extension : kNoteSiblings) {
    String sibling = base + extension;
    if (fs().exists(sibling) && !fs().remove(sibling)) return false;
  }
  if (fs().exists(temporary) && !fs().remove(temporary)) return false;
  if (fs().exists(tombstone) && !fs().remove(tombstone)) return false;
  return true;
}

bool readBounded(File &file, String &out, size_t maximum) {
  out = "";
  if (!file || file.isDirectory() || file.size() > maximum || !file.seek(0)) return false;
  size_t expected = file.size(), remaining = expected;
  if (expected && !out.reserve(expected)) return false;
  uint8_t buffer[512];
  while (remaining) {
    size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t got = file.read(buffer, wanted);
    if (!got || got > wanted || !out.concat(reinterpret_cast<const char *>(buffer), got)) {
      out = ""; return false;
    }
    remaining -= got;
  }
  if (file.size() != expected || out.length() != expected) { out = ""; return false; }
  return true;
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
  File file = openRead(path);
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
