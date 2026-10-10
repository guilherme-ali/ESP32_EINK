#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#include <vector>
#include <ctime>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace Host { inline uint64_t fileReadCalls = 0; }
using boolean = bool;
namespace fs {
class FileImpl;
using FileImplPtr = std::shared_ptr<FileImpl>;
class FSImpl;
using FSImplPtr = std::shared_ptr<FSImpl>;
enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };
}
#include "FSImpl.h"

namespace fs {
struct FileStats {
  size_t reads = 0, writes = 0, seeks = 0, writableSeeks = 0, rewrittenBytes = 0;
  size_t writtenBytes = 0, flushes = 0, closes = 0;
  std::vector<size_t> writeSizes, flushSizes;
  std::vector<std::string> writePrefixes; // ate 44 bytes; nao duplica payload longo.
  std::vector<std::string> openModes;
};
struct FileEvent {
  std::string operation, path;
  size_t value;
};
struct Faults {
  size_t readLimit = SIZE_MAX, writeLimit = SIZE_MAX, readBudget = SIZE_MAX;
  std::string failOpen, failRename, failRemove, corruptFlush;
  size_t flushes = 0, renames = 0;
  size_t corruptFlushAfter = 0, failRenameAfter = 0;
  // Configurar antes de liberar as tasks; inspecionar somente depois do join.
  std::map<std::string, size_t> pathWriteLimits, pathWriteBudgets;
  std::string truncateFlush;
  size_t truncateFlushBytes = 1;
  std::map<std::string, FileStats> files;
  std::vector<FileEvent> events;
};
class File {
  FileImplPtr impl_;
  std::shared_ptr<std::vector<uint8_t>> data_;
  size_t position_ = 0;
  size_t readLimit_ = SIZE_MAX;
  bool writable_ = false;
  std::shared_ptr<Faults> faults_;
  std::string path_;
  bool append_ = false;

public:
  File() = default;
  File(FileImplPtr impl) : impl_(std::move(impl)) {}
  explicit File(const std::string &data, size_t readLimit = SIZE_MAX)
      : data_(std::make_shared<std::vector<uint8_t>>(data.begin(), data.end())), readLimit_(readLimit) {}
  File(std::shared_ptr<std::vector<uint8_t>> data, bool writable,
       std::shared_ptr<Faults> faults, std::string path, bool append = false)
      : data_(std::move(data)), writable_(writable), faults_(std::move(faults)), path_(std::move(path)), append_(append) {
    if (append_) position_ = size(); // abrir append nao e seek do chamador.
  }
  operator bool() const { return impl_ ? static_cast<bool>(*impl_) : static_cast<bool>(data_); }
  bool isDirectory() const { return impl_ ? impl_->isDirectory() : false; }
  size_t size() const { return impl_ ? impl_->size() : (data_ ? data_->size() : 0); }
  size_t position() const { return impl_ ? impl_->position() : position_; }
  bool seek(uint32_t offset, SeekMode mode = SeekSet) {
    if (impl_) return impl_->seek(offset, mode);
    if (faults_) {
      ++faults_->files[path_].seeks;
      if (writable_) ++faults_->files[path_].writableSeeks;
      faults_->events.push_back({"seek", path_, offset});
    }
    size_t target = offset;
    if (mode == SeekCur) target += position_;
    else if (mode == SeekEnd) target += size();
    else if (mode != SeekSet) return false;
    if (!data_ || target > data_->size()) return false;
    position_ = target; return true;
  }
  int available() const { return static_cast<int>(size() >= position() ? size() - position() : 0); }
  size_t read(uint8_t *out, size_t count) {
    ++Host::fileReadCalls;
    if (impl_) return impl_->read(out, count);
    if (!data_) return 0;
    size_t n = std::min({count, size() - std::min(position_, size()), readLimit_});
    if (faults_) {
      ++faults_->files[path_].reads;
      n = std::min({n, faults_->readLimit, faults_->readBudget});
      if (faults_->readBudget != SIZE_MAX) faults_->readBudget -= n;
    }
    if (n) std::memcpy(out, data_->data() + position_, n);
    position_ += n; return n;
  }
  int read() { uint8_t c; return read(&c, 1) ? c : -1; }
  int peek() {
    size_t old = position(); int c = read();
    if (c >= 0) seek(static_cast<uint32_t>(old));
    return c;
  }
  size_t readBytes(char *out, size_t count) {
    if (impl_) return read(reinterpret_cast<uint8_t *>(out), count);
    size_t total = 0, n;
    while (total < count && (n = read(reinterpret_cast<uint8_t *>(out + total), count - total))) total += n;
    return total;
  }
  String readString() {
    std::string result;
    for (int c; available() && (c = read()) >= 0;) result += static_cast<char>(c);
    // File do core 2.0.17 usa timeout zero; readString ainda le byte a byte.
    return String(result);
  }
  size_t write(const uint8_t *data, size_t count) {
    if (impl_) return impl_->write(data, count);
    if (!data_ || !writable_) return 0;
    if (append_) position_ = size();
    if (faults_) {
      count = std::min(count, faults_->writeLimit);
      auto limit = faults_->pathWriteLimits.find(path_);
      if (limit != faults_->pathWriteLimits.end()) count = std::min(count, limit->second);
      auto budget = faults_->pathWriteBudgets.find(path_);
      if (budget != faults_->pathWriteBudgets.end()) {
        count = std::min(count, budget->second); budget->second -= count;
      }
      auto &stats = faults_->files[path_];
      ++stats.writes; stats.writtenBytes += count; stats.writeSizes.push_back(count);
      stats.writePrefixes.emplace_back(reinterpret_cast<const char *>(data), std::min(count, size_t(44)));
      stats.rewrittenBytes += std::min(count, size() - std::min(position_, size()));
      faults_->events.push_back({"write", path_, count});
    }
    if (position_ + count > size()) data_->resize(position_ + count);
    if (count) std::memcpy(data_->data() + position_, data, count);
    position_ += count; return count;
  }
  size_t write(uint8_t value) { return write(&value, 1); }
  size_t print(const String &value) {
    return write(reinterpret_cast<const uint8_t *>(value.c_str()), value.length());
  }
  void flush() {
    if (impl_) { impl_->flush(); return; }
    if (!data_ || !faults_) return;
    ++faults_->flushes;
    auto &stats = faults_->files[path_]; ++stats.flushes; stats.flushSizes.push_back(size());
    faults_->events.push_back({"flush", path_, size()});
    if (path_ == faults_->corruptFlush && stats.flushes >= faults_->corruptFlushAfter && !data_->empty()) (*data_)[0] = 0;
    if (path_ == faults_->truncateFlush && !data_->empty())
      data_->resize(size() - std::min(size(), faults_->truncateFlushBytes));
  }
  void close() {
    if (impl_) { impl_->close(); impl_.reset(); }
    if (data_ && faults_) {
      ++faults_->files[path_].closes;
      faults_->events.push_back({"close", path_, size()});
    }
    data_.reset(); position_ = 0;
  }
  bool setBufferSize(size_t n) { return impl_ ? impl_->setBufferSize(n) : static_cast<bool>(data_); }
  time_t getLastWrite() { return impl_ ? impl_->getLastWrite() : 0; }
  const char *path() const { return impl_ ? impl_->path() : (data_ ? path_.c_str() : nullptr); }
  const char *name() const {
    if (impl_) return impl_->name();
    const char *p = path(); if (!p) return nullptr;
    const char *slash = std::strrchr(p, '/'); return slash ? slash + 1 : p;
  }
  File openNextFile(const char *mode = FILE_READ) { return impl_ ? File(impl_->openNextFile(mode)) : File(); }
  bool seekDir(long n) { return impl_ && impl_->seekDir(n); }
  String getNextFileName() { return impl_ ? impl_->getNextFileName() : String(); }
  String getNextFileName(bool *isDir) { return impl_ ? impl_->getNextFileName(isDir) : String(); }
  void rewindDirectory() { if (impl_) impl_->rewindDirectory(); }
};

class FS {
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files_;

public:
  std::shared_ptr<Faults> faults = std::make_shared<Faults>();
  void put(const char *path, const std::string &data) {
    files_[path] = std::make_shared<std::vector<uint8_t>>(data.begin(), data.end());
  }
  void clear() { files_.clear(); faults = std::make_shared<Faults>(); }
  uint64_t usedBytes() const {
    uint64_t n = 0; for (const auto &entry : files_) n += entry.second->size(); return n;
  }
  std::string bytes(const char *path) const {
    auto it = files_.find(path);
    return it == files_.end() ? std::string() : std::string(it->second->begin(), it->second->end());
  }
  File open(const char *path, const char *mode = FILE_READ, const bool create = false) {
    (void)create;
    faults->files[path].openModes.push_back(mode);
    faults->events.push_back({std::string("open:") + mode, path, 0});
    if (path == faults->failOpen) return File();
    bool writable = std::strcmp(mode, FILE_READ) != 0;
    auto found = files_.find(path);
    if (!writable && found == files_.end()) return File();
    if (writable && (std::strcmp(mode, FILE_WRITE) == 0 || found == files_.end())) put(path, "");
    File result(files_.at(path), writable, faults, path, std::strcmp(mode, FILE_APPEND) == 0);
    return result;
  }
  File open(const String &path, const char *mode = FILE_READ, const bool create = false) { return open(path.c_str(), mode, create); }
  bool exists(const char *path) const { return files_.count(path) != 0; }
  bool exists(const String &path) const { return exists(path.c_str()); }
  bool remove(const char *path) {
    faults->events.push_back({"remove", path, 0});
    return path != faults->failRemove && files_.erase(path) != 0;
  }
  bool remove(const String &path) { return remove(path.c_str()); }
  bool rename(const char *from, const char *to) {
    ++faults->renames;
    faults->events.push_back({"rename", from, 0});
    if (from == faults->failRename && faults->renames >= faults->failRenameAfter) return false;
    auto found = files_.find(from);
    if (found == files_.end()) return false;
    auto destination = files_.find(to);
    if (found->second.use_count() > 1 ||
        (destination != files_.end() && destination->second.use_count() > 1)) return false;
    if (std::strcmp(from, to) != 0) { files_[to] = found->second; files_.erase(found); }
    return true;
  }
  bool rename(const String &from, const String &to) { return rename(from.c_str(), to.c_str()); }
};
} // namespace fs
using fs::File;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;
