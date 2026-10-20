#pragma once
#include <FS.h>

// A suite portavel nao executa gravacao nem limpeza. As secoes do comando
// completo sao descartadas pelo linker; estas assinaturas apenas as compilam.
class PowerSelfTestFS : public fs::FS {
public:
  bool mkdir(const char *) { return false; }
  bool rmdir(const char *) { return false; }
};
inline PowerSelfTestFS LittleFS;
