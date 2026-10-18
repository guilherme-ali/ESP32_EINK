#include "phone_voice.h"
#if defined(ARDUINO_ARCH_ESP32)
#include "platform.h"
#include "live_protocol.h"
#include "../connectivity/ble_relay.h"
#include <algorithm>
#include <cstring>

namespace Assistant {
PhoneVoice::PhoneVoice(uint8_t *p, size_t cap, Stop stop, void *ctx)
  : storage_(p), capacity_(cap), stop_(stop), context_(ctx) {}
PhoneVoice::~PhoneVoice() { close(); memset(input_, 0, sizeof(input_)); }
bool PhoneVoice::fail(const char *error) { error_ = error; close(); return false; }
bool PhoneVoice::flushOpus() {
  if (channel_ < 0 || stop_(context_)) { close(); return false; }
  if (!opusBatchSize_) return true;
  const size_t count = opusBatchSize_;
  opusBatchSize_ = 0; // Never replay a batch after a partial transport write.
  const size_t done = PhoneRelay::write(channel_, opusBatch_, count, 5000);
  sentBytes_ += done;
  if (done != count) return fail("Midia BLE interrompida durante envio; sem replay.");
  return true;
}
bool PhoneVoice::record(Media::Type type, const uint8_t *p, size_t n, uint32_t position, uint8_t flags) {
  if (channel_ < 0 || n > UINT32_MAX || stop_(context_)) { close(); return false; }
  Media::Header h; h.type = type; h.flags = flags; h.length = static_cast<uint32_t>(n); h.position = position;
  if (type == Media::Type::Opus) {
    if (n > sizeof(opusBatch_) - Media::kHeaderSize) return fail("Pacote Opus acima do lote de midia.");
    const size_t count = Media::kHeaderSize + n;
    if (count > sizeof(opusBatch_) - opusBatchSize_ && !flushOpus()) return false;
    Media::encodeHeader(h, opusBatch_ + opusBatchSize_);
    if (n) memcpy(opusBatch_ + opusBatchSize_ + Media::kHeaderSize, p, n);
    opusBatchSize_ += count;
    return opusBatchSize_ != sizeof(opusBatch_) || flushOpus();
  }
  if (opusBatchSize_ && !flushOpus()) return false;
  uint8_t chunk[1384];
  Media::encodeHeader(h, chunk);
  const size_t first = std::min(n, sizeof(chunk) - Media::kHeaderSize);
  if (first) memcpy(chunk + Media::kHeaderSize, p, first);
  const uint32_t start = millis();
  size_t at = 0, count = Media::kHeaderSize + first;
  while (!stop_(context_)) {
      if (uint32_t(millis() - start) >= 10000) return fail("Timeout ao enviar midia BLE.");
       const size_t done = PhoneRelay::write(channel_, chunk, count, 5000);
       sentBytes_ += done;
       if (done != count) return fail("Midia BLE interrompida durante envio; sem replay.");
      at += at ? count : first;
      if (at == n) return true;
      count = std::min(n - at, sizeof(chunk));
      memcpy(chunk, p + at, count);
  }
  return false;
}
bool PhoneVoice::connect(const char *key, int bitrate) {
  bitrate_ = bitrate;
  if (!storage_ || capacity_ < Media::kJsonLimit + 1 || (bitrate && !opus_.begin(bitrate)))
    return fail("Codec Opus indisponivel ou sem memoria.");
  channel_ = PhoneRelay::open(Media::kEndpoint, 443, 10000);
  if (channel_ < 0) return fail("Atualize o companion para midia BLE v1 ou use legacy.");
  Json json(cJSON_CreateObject(), cJSON_Delete);
  cJSON_AddNumberToObject(json.get(), "version", 1);
  cJSON_AddStringToObject(json.get(), "codec", bitrate ? "opus" : "pcm");
  cJSON_AddNumberToObject(json.get(), "bitrate", bitrate);
  cJSON_AddNumberToObject(json.get(), "inputRate", 16000);
  cJSON_AddNumberToObject(json.get(), "outputRate", 24000);
  cJSON_AddNumberToObject(json.get(), "frameMs", 20);
  cJSON_AddNumberToObject(json.get(), "inputLookahead", bitrate ? opus_.encoderLookahead() : 0);
  cJSON_AddStringToObject(json.get(), "key", key);
  std::string body = printJson(json.get());
  const bool sent = record(Media::Type::Hello, reinterpret_cast<const uint8_t *>(body.data()), body.size());
  std::fill(body.begin(), body.end(), '\0');
  cJSON *secret = cJSON_GetObjectItemCaseSensitive(json.get(), "key");
  if (secret && secret->valuestring) memset(secret->valuestring, 0, strlen(secret->valuestring));
  if (!sent) return false;
  const uint32_t start = millis();
  while (!stop_(context_) && uint32_t(millis() - start) < 15000) {
    const auto event = poll();
    if (event == WsParser::Event::Error) return false;
    if (hello_) return true;
    if (event != WsParser::Event::None) return fail("Midia recebida antes da negociacao.");
    vTaskDelay(1);
  }
  return fail("Timeout na negociacao de midia com celular.");
}
bool PhoneVoice::sendJson(const std::string &json) {
  return hello_ && record(Media::Type::Json, reinterpret_cast<const uint8_t *>(json.data()), json.size());
}
bool PhoneVoice::encodeFrame() {
  uint8_t payload[Media::kPacketLimit + 4];
  const uint64_t begin = assistantMonotonicUs();
  const int n = opus_.encode(input_, 320, payload + 4, Media::kPacketLimit);
  encodeUs_ += static_cast<uint32_t>(assistantMonotonicUs() - begin);
  if (n <= 0) return fail("Falha ao codificar microfone em Opus.");
  const auto cut = Media::trim(encoded_, original_, static_cast<uint16_t>(opus_.encoderLookahead()), 320);
  payload[0] = uint8_t(cut.skip); payload[1] = uint8_t(cut.skip >> 8);
  payload[2] = uint8_t(cut.take); payload[3] = uint8_t(cut.take >> 8);
  if (!record(Media::Type::Opus, payload, size_t(n) + 4, emitted_, first_ ? 1 : 0)) return false;
  first_ = false; encoded_ += 320; emitted_ += cut.take;
  return true;
}
bool PhoneVoice::sendAudio(const uint8_t *pcm, size_t n) {
  if (!hello_ || ended_ || (n & 1) || !n) return fail("Estado de captura de midia invalido.");
  if (!bitrate_) {
    if (!record(Media::Type::Pcm, pcm, n, original_)) return false;
    original_ += n / 2; return true;
  }
  for (size_t at = 0; at < n;) {
    const size_t take = std::min(n - at, (320 - staged_) * 2);
    memcpy(input_ + staged_, pcm + at, take);
    staged_ += take / 2; original_ += take / 2; at += take;
    if (staged_ == 320) { if (!encodeFrame()) return false; staged_ = 0; }
  }
  return flushOpus();
}
bool PhoneVoice::endInput() {
  if (!hello_ || ended_ || !original_) return fail("Fim de captura de midia invalido.");
  if (bitrate_) {
    if (staged_) {
      memset(input_ + staged_, 0, (320 - staged_) * sizeof(int16_t));
      if (!encodeFrame()) return false;
      staged_ = 0;
    }
    while (encoded_ < original_ + static_cast<uint32_t>(opus_.encoderLookahead())) {
      memset(input_, 0, sizeof(input_)); if (!encodeFrame()) return false;
    }
    if (emitted_ != original_) return fail("Cauda de captura Opus incompleta.");
  }
  ended_ = true;
  return record(Media::Type::EndInput, nullptr, 0, original_);
}
WsParser::Event PhoneVoice::poll() {
  using E = WsParser::Event;
  audioResult_ = false; resultSize_ = 0;
  if (channel_ < 0) return E::Error;
  if (headerAt_ && uint32_t(millis() - pendingSince_) >= 20000) { fail("Timeout no frame de midia BLE."); return E::Error; }
  for (size_t budget = 4096; budget && !stop_(context_);) {
    if (headerAt_ < Media::kHeaderSize) {
      const size_t take = std::min(budget, Media::kHeaderSize - headerAt_);
      const int got = PhoneRelay::read(channel_, wire_ + headerAt_, take);
      if (got <= 0) break;
      const size_t n = static_cast<size_t>(got);
      if (n > take) { fail("Leitura de midia acima do solicitado."); return E::Error; }
      if (!headerAt_) pendingSince_ = millis();
      headerAt_ += n; receivedBytes_ += n; budget -= n;
      if (headerAt_ < Media::kHeaderSize) continue;
      if (!Media::decodeHeader(wire_, header_) || header_.length >= capacity_) {
        fail("Cabecalho de midia BLE invalido."); return E::Error;
      }
    }
    const size_t requested = std::min(budget, size_t(header_.length) - payloadAt_);
    const int got = requested ? PhoneRelay::read(channel_, storage_ + payloadAt_, requested) : 0;
    const size_t n = got > 0 ? static_cast<size_t>(got) : 0;
    if (n > requested) { fail("Leitura de midia acima do solicitado."); return E::Error; }
    payloadAt_ += n; receivedBytes_ += n; budget -= n;
    if (payloadAt_ != header_.length) { if (!n) break; continue; }
    storage_[payloadAt_] = 0;
    headerAt_ = payloadAt_ = 0;
    if (header_.type == Media::Type::Error) { fail("Celular recusou midia: codec, acesso, modelo ou cota."); return E::Error; }
    if (!hello_) {
      Json hello = parseJson(reinterpret_cast<const char *>(storage_), header_.length, 2048);
      const cJSON *root = hello.get();
      auto number = [&](const char *name) { const cJSON *x = field(root, name); return cJSON_IsNumber(x) ? x->valueint : -1; };
      if (header_.type != Media::Type::Hello || !hello || number("version") != 1 ||
          strcmp(stringField(root, "codec"), bitrate_ ? "opus" : "pcm") || number("bitrate") != bitrate_ ||
          number("inputRate") != 16000 || number("outputRate") != 24000 || number("frameMs") != 20 ||
          number("outputLookahead") < 0 || number("outputLookahead") > 480) {
        fail("Perfil de midia BLE nao corresponde ao solicitado."); return E::Error;
      }
      outputLookahead_ = static_cast<uint16_t>(number("outputLookahead"));
      hello_ = true; return E::None;
    }
    if (header_.type == Media::Type::Json) {
      Json json = parseJson(reinterpret_cast<const char *>(storage_), header_.length);
      if (!json) { fail("JSON de controle de midia invalido."); return E::Error; }
      if (trueField(field(json.get(), "serverContent"), "turnComplete")) outputResetAllowed_ = true;
      resultSize_ = header_.length; return E::Message;
    }
    if (header_.type == Media::Type::Pcm && !bitrate_) {
      if (header_.position != receivedSamples_) { fail("PCM de midia fora de ordem."); return E::Error; }
      receivedSamples_ += header_.length / 2; resultSize_ = header_.length; return E::Audio;
    }
    if (header_.type != Media::Type::Opus || !bitrate_) { fail("Tipo de midia nao negociado."); return E::Error; }
    const uint16_t skip = uint16_t(storage_[0]) | uint16_t(storage_[1]) << 8;
    const uint16_t take = uint16_t(storage_[2]) | uint16_t(storage_[3]) << 8;
    if (!Media::validAudio(header_, receivedSamples_, skip, take, 480) ||
        (header_.flags && !outputResetAllowed_) || (outputResetAllowed_ && !header_.flags)) {
      fail("Sequencia ou recorte Opus invalido."); return E::Error;
    }
    if (header_.flags) {
      if (skip != outputLookahead_) { fail("Pre-skip Opus nao negociado."); return E::Error; }
      opus_.resetDecoder(); outputResetAllowed_ = false;
    } else if (skip) { fail("Pre-skip Opus fora do inicio de segmento."); return E::Error; }
    const uint64_t begin = assistantMonotonicUs();
    const int decoded = opus_.decode(storage_ + 4, header_.length - 4, decoded_, 480);
    decodeUs_ += static_cast<uint32_t>(assistantMonotonicUs() - begin);
    if (decoded != 480) {
      fail("Falha no decoder Opus; sem ocultar perdas."); return E::Error;
    }
    if (take) memmove(decoded_, decoded_ + skip, take * sizeof(int16_t));
    receivedSamples_ += take; resultSize_ = take * sizeof(int16_t); audioResult_ = true;
    if (take) return E::Audio;
  }
  if (!PhoneRelay::connected(channel_) && !PhoneRelay::available(channel_)) { fail("Canal de midia encerrado antes da resposta."); return E::Error; }
  return E::None;
}
bool PhoneVoice::pending() { return headerAt_ || (channel_ >= 0 && PhoneRelay::available(channel_) > 0); }
void PhoneVoice::close() {
  opusBatchSize_ = 0;
  if (channel_ >= 0) { PhoneRelay::close(channel_); channel_ = -1; }
}
}
#endif
