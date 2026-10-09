#include <FS.h>
#include <mbedtls/sha256.h>

void Host::fakeKeyHash(const unsigned char *data, size_t count, unsigned char out[32]) {
  // FNV lane salt; somente separacao deterministica de chaves dos testes.
  for (unsigned lane = 0; lane < 8; ++lane) {
    uint32_t value = 2166136261U ^ (lane * 0x9e3779b9U);
    for (size_t i = 0; i < count; ++i) value = (value ^ data[i]) * 16777619U;
    for (unsigned j = 0; j < 4; ++j) out[4 * lane + j] = static_cast<unsigned char>(value >> (8 * j));
  }
}
extern "C" int mbedtls_sha256_ret(const unsigned char *data, size_t count, unsigned char *out, int sha224) {
  if (!Host::fakeShaEnabled || sha224) return -1;
  Host::fakeKeyHash(data, count, out);
  return 0;
}
