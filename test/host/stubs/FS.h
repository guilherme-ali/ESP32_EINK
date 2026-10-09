#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#include <vector>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace fs {
struct Faults {
  size_t readLimit = SIZE_MAX, writeLimit = SIZE_MAX, readBudget = SIZE_MAX;
  std::string failOpen, failRename, corruptFlush;
  size_t flushes = 0, renames = 0;
};
class File {
  std::shared_ptr<std::vector<uint8_t>> data_;
  size_t position_ = 0;
  size_t readLimit_ = SIZE_MAX;
  bool writable_ = false;
  std::shared_ptr<Faults> faults_;
  std::string path_;

public:
  File() = default;
  explicit File(const std::string &data, size_t readLimit = SIZE_MAX)
      : data_(std::make_shared<std::vector<uint8_t>>(data.begin(), data.end())), readLimit_(readLimit) {}
  File(std::shared_ptr<std::vector<uint8_t>> data, bool writable,
       std::shared_ptr<Faults> faults, std::string path)
      : data_(std::move(data)), writable_(writable), faults_(std::move(faults)), path_(std::move(path)) {}
  explicit operator bool() const { return static_cast<bool>(data_); }
  bool isDirectory() const { return false; }
  size_t size() const { return data_ ? data_->size() : 0; }
  size_t position() const { return position_; }
  bool seek(size_t offset) {
    if (!data_ || offset > data_->size()) return false;
    position_ = offset; return true;
  }
  int available() const { return static_cast<int>(size() - position_); }
  size_t read(uint8_t *out, size_t count) {
    if (!data_) return 0;
    size_t n = std::min({count, size() - position_, readLimit_});
    if (faults_) {
      n = std::min({n, faults_->readLimit, faults_->readBudget});
      if (faults_->readBudget != SIZE_MAX) faults_->readBudget -= n;
    }
    if (n) std::memcpy(out, data_->data() + position_, n);
    position_ += n; return n;
  }
  int read() { uint8_t c; return read(&c, 1) ? c : -1; }
  size_t readBytes(char *out, size_t count) {
    size_t total = 0, n;
    while (total < count && (n = read(reinterpret_cast<uint8_t *>(out + total), count - total))) total += n;
    return total;
  }
  String readString() {
    std::string result;
    for (int c; available() && (c = read()) >= 0;) result += static_cast<char>(c);
    return String(result);
  }
  size_t write(const uint8_t *data, size_t count) {
    if (!data_ || !writable_) return 0;
    if (faults_) count = std::min(count, faults_->writeLimit);
    if (position_ + count > size()) data_->resize(position_ + count);
    if (count) std::memcpy(data_->data() + position_, data, count);
    position_ += count; return count;
  }
  size_t print(const String &value) {
    return write(reinterpret_cast<const uint8_t *>(value.c_str()), value.length());
  }
  void flush() {
    if (!data_ || !faults_) return;
    ++faults_->flushes;
    if (path_ == faults_->corruptFlush && !data_->empty()) (*data_)[0] = 0;
  }
  void close() { data_.reset(); position_ = 0; }
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
  File open(const char *path, const char *mode = FILE_READ) {
    if (path == faults->failOpen) return File();
    bool writable = std::strcmp(mode, FILE_READ) != 0;
    auto found = files_.find(path);
    if (!writable && found == files_.end()) return File();
    if (writable && (std::strcmp(mode, FILE_WRITE) == 0 || found == files_.end())) put(path, "");
    File result(files_.at(path), writable, faults, path);
    if (std::strcmp(mode, FILE_APPEND) == 0) result.seek(result.size());
    return result;
  }
  File open(const String &path, const char *mode = FILE_READ) { return open(path.c_str(), mode); }
  bool exists(const char *path) const { return files_.count(path) != 0; }
  bool exists(const String &path) const { return exists(path.c_str()); }
  bool remove(const char *path) { return files_.erase(path) != 0; }
  bool remove(const String &path) { return remove(path.c_str()); }
  bool rename(const char *from, const char *to) {
    ++faults->renames;
    if (from == faults->failRename) return false;
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
