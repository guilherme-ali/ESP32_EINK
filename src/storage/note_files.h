#pragma once
#include <Arduino.h>
#include <FS.h>

// Backend das notas. A flash continua armazenando wallpapers/configuracoes;
// um futuro backend SD pode implementar este contrato sem alterar os clientes.
namespace NoteFiles {
constexpr size_t kReserveBytes = 256 * 1024;
constexpr size_t kMaxTextBytes = 128 * 1024 - 1;
fs::FS &fs();
uint64_t totalBytes();
uint64_t usedBytes();
uint64_t freeBytes();
uint64_t recordingBytes();
bool validText(const char *path);
bool validText(const String &path);
bool validWav(const char *path);
bool readText(const String &path, char *out, size_t capacity);
// Le por blocos o tamanho conhecido, evitando readString byte a byte.
bool readBounded(File &file, String &out, size_t maximum);
bool writeAtomic(const String &path, const char *data, size_t length);
void recoverText(const String &path);
}
