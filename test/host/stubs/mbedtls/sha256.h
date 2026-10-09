#pragma once
#include <cstddef>

// Intentionally NOT crypto. A call fails instead of pretending to hash a key.
// Parser/compatible-provider tests never enter Gemini's account activation.
extern "C" int mbedtls_sha256_ret(const unsigned char *, size_t, unsigned char *, int);
