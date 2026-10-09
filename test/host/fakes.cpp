#include <FS.h>
#include <mbedtls/sha256.h>

extern "C" int mbedtls_sha256_ret(const unsigned char *, size_t, unsigned char *, int) {
  return -1;
}
