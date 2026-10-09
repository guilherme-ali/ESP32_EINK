#pragma once
#include <cstddef>

// NOT crypto: default failure. Integration opts in to a deterministic keyed
// fingerprint, exercising production cache/NVS without using real credentials.
namespace Host {
inline bool fakeShaEnabled = false;
void fakeKeyHash(const unsigned char *data, size_t count, unsigned char out[32]);
}
extern "C" int mbedtls_sha256_ret(const unsigned char *, size_t, unsigned char *, int);
