#include "media_protocol.h"
#include <algorithm>

namespace Assistant { namespace Media {
static uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (i * 8));
}
void encodeHeader(const Header &h, uint8_t *out) {
  out[0] = 0x56; out[1] = 0x31; out[2] = static_cast<uint8_t>(h.type); out[3] = h.flags;
  put32(out + 4, h.length); put32(out + 8, h.position);
}
bool decodeHeader(const uint8_t *p, Header &h) {
  if (!p || p[0] != 0x56 || p[1] != 0x31 || p[2] < 1 || p[2] > 6) return false;
  h.type = static_cast<Type>(p[2]); h.flags = p[3]; h.length = get32(p + 4); h.position = get32(p + 8);
  if (h.flags && (h.type != Type::Opus || h.flags != 1)) return false;
  switch (h.type) {
    case Type::Opus: return h.length > 4 && h.length <= kPacketLimit + 4;
    case Type::Pcm: return h.length && !(h.length & 1) && h.length <= 192 * 1024;
    case Type::EndInput: return h.length == 0;
    case Type::Hello: return h.length > 0 && h.length <= 2048 && h.position == 0;
    case Type::Json: return h.length > 0 && h.length <= kJsonLimit && h.position == 0;
    case Type::Error: return h.length <= 1024 && h.position == 0;
  }
  return false;
}
Trim trim(uint32_t encoded, uint32_t original, uint16_t lookahead, uint16_t frame) {
  const uint32_t skip = encoded < lookahead ? std::min<uint32_t>(lookahead - encoded, frame) : 0;
  const uint32_t start = encoded > lookahead ? encoded - lookahead : 0;
  const uint64_t right = uint64_t(encoded) + frame;
  const uint32_t end = right > lookahead ? static_cast<uint32_t>(std::min<uint64_t>(right - lookahead, original)) : 0;
  return {static_cast<uint16_t>(skip), static_cast<uint16_t>(end > start ? std::min<uint32_t>(end - start, frame - skip) : 0)};
}
bool validAudio(const Header &h, uint32_t expected, uint16_t skip, uint16_t take, uint16_t frame) {
  return h.position == expected && skip <= frame && take <= frame - skip &&
    uint64_t(expected) + take <= UINT32_MAX;
}
} }
