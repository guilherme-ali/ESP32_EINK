#include "../../src/assistant/media_protocol.h"
#include <cstdlib>
#include <iostream>
#include <cstring>
using namespace Assistant::Media;
static size_t checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { std::cerr << __LINE__ << ": " #condition "\n"; std::exit(1); } } while (0)
int main() {
  Header h; h.type = Type::Opus; h.flags = 1; h.length = 164; h.position = 0x12345678;
  uint8_t wire[kHeaderSize]; encodeHeader(h, wire);
  const uint8_t golden[] = {0x56,0x31,3,1,164,0,0,0,0x78,0x56,0x34,0x12};
  CHECK(!memcmp(wire, golden, sizeof(golden)));
  Header decoded; CHECK(decodeHeader(wire, decoded));
  CHECK(decoded.position == h.position && decoded.length == h.length && decoded.flags == 1);
  CHECK(validAudio(h, h.position, 104, 216, 320));
  CHECK(!validAudio(h, h.position + 1, 104, 216, 320));
  CHECK(!validAudio(h, h.position, 104, 217, 320));
  CHECK(!validAudio(h, h.position, 321, 0, 320));
  for (size_t byte : {size_t(0), size_t(1), size_t(2), size_t(3)}) {
    const uint8_t before = wire[byte]; wire[byte] = 255;
    CHECK(!decodeHeader(wire, decoded)); wire[byte] = before;
  }
  h.flags = 0; h.type = Type::Pcm; h.position = 0; h.length = 960;
  encodeHeader(h, wire); CHECK(decodeHeader(wire, decoded));
  h.length = 959; encodeHeader(h, wire); CHECK(!decodeHeader(wire, decoded));
  h.type = Type::Json; h.length = kJsonLimit; encodeHeader(h, wire); CHECK(decodeHeader(wire, decoded));
  h.length++; encodeHeader(h, wire); CHECK(!decodeHeader(wire, decoded));
  h.type = Type::EndInput; h.length = 0; h.position = 16000;
  encodeHeader(h, wire); CHECK(decodeHeader(wire, decoded));
  h.length = 1; encodeHeader(h, wire); CHECK(!decodeHeader(wire, decoded));
  h.type = Type::Hello; h.length = 200; h.position = 0;
  encodeHeader(h, wire); CHECK(decodeHeader(wire, decoded));
  h.position = 1; encodeHeader(h, wire); CHECK(!decodeHeader(wire, decoded));
  // Exact preservation of the original sample count, including a one-sample
  // final tail, across encoder delay/padding at both supported sample rates.
  for (uint16_t frame : {uint16_t(320), uint16_t(480)}) {
    for (uint16_t lookahead : {uint16_t(0), uint16_t(104), uint16_t(156), uint16_t(312), uint16_t(600)}) {
      for (uint32_t original = 1; original < 10000; original += 17) {
        uint32_t emitted = 0;
        for (uint32_t encoded = 0; encoded < original + lookahead; encoded += frame) {
          const Trim cut = trim(encoded, original, lookahead, frame);
          CHECK(cut.skip <= frame && cut.take <= frame - cut.skip);
          const uint32_t logical = encoded > lookahead ? encoded - lookahead : 0;
          if (cut.take) CHECK(logical == emitted);
          emitted += cut.take;
        }
        CHECK(emitted == original);
      }
    }
  }
  std::cout << "Media v1: " << checks << " verificacoes (wire, bounds, ordem, lookahead e cauda) OK\n";
}
