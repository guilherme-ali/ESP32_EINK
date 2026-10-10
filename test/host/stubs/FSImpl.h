#ifndef FSIMPL_H
#define FSIMPL_H

// Contrato do libraries/FS/src/FSImpl.h do Arduino-ESP32 2.0.17
// instalado. Como no SDK, incluir FS.h antes deste arquivo.
#include <stddef.h>
#include <stdint.h>
#include <ctime>

namespace fs {
class FileImpl {
public:
  virtual ~FileImpl() {}
  virtual size_t write(const uint8_t *buf, size_t size) = 0;
  virtual size_t read(uint8_t *buf, size_t size) = 0;
  virtual void flush() = 0;
  virtual bool seek(uint32_t pos, SeekMode mode) = 0;
  virtual size_t position() const = 0;
  virtual size_t size() const = 0;
  virtual bool setBufferSize(size_t size) = 0;
  virtual void close() = 0;
  virtual time_t getLastWrite() = 0;
  virtual const char *path() const = 0;
  virtual const char *name() const = 0;
  virtual boolean isDirectory(void) = 0;
  virtual FileImplPtr openNextFile(const char *mode) = 0;
  virtual boolean seekDir(long position) = 0;
  virtual String getNextFileName(void) = 0;
  virtual String getNextFileName(bool *isDir) = 0;
  virtual void rewindDirectory(void) = 0;
  virtual operator bool() = 0;
};

class FSImpl {
protected:
  const char *_mountpoint;
public:
  FSImpl() : _mountpoint(NULL) {}
  virtual ~FSImpl() {}
  virtual FileImplPtr open(const char *path, const char *mode, const bool create) = 0;
  virtual bool exists(const char *path) = 0;
  virtual bool rename(const char *pathFrom, const char *pathTo) = 0;
  virtual bool remove(const char *path) = 0;
  virtual bool mkdir(const char *path) = 0;
  virtual bool rmdir(const char *path) = 0;
  void mountpoint(const char *point) { _mountpoint = point; }
  const char *mountpoint() { return _mountpoint; }
};
} // namespace fs
#endif
