#pragma once
#include <cstddef>
#include <cstdint>

namespace Assistant { namespace Media {
constexpr size_t kHeaderSize = 12, kPacketLimit = 1276, kJsonLimit = 256 * 1024;
constexpr const char *kEndpoint = "voice-v1.ideiarec.invalid";
enum class Type : uint8_t { Hello = 1, Json = 2, Opus = 3, Pcm = 4, EndInput = 5, Error = 6 };
struct Header { Type type = Type::Error; uint8_t flags = 0; uint32_t length = 0, position = 0; };
void encodeHeader(const Header &header, uint8_t *out);
bool decodeHeader(const uint8_t *data, Header &header);
// PCM coordinates after removing encoder delay/padding, not packet indices.
struct Trim { uint16_t skip, take; };
Trim trim(uint32_t encoded, uint32_t original, uint16_t lookahead, uint16_t frame);
bool validAudio(const Header &header, uint32_t expected, uint16_t skip, uint16_t take, uint16_t frame);
} }
