#include "transport.h"
#include "live_protocol.h"
#include <algorithm>
#include <cstring>

namespace Assistant {
static bool expired(uint32_t begin, uint32_t limit) { return uint32_t(millis() - begin) >= limit; }
static bool writeBytes(NetworkClient &client, const uint8_t *p, size_t n,
                       StopFn stop, void *context, uint32_t timeout) {
  const uint32_t begin = millis(); size_t at = 0;
  while (at < n) {
    if (stop(context) || expired(begin, timeout) || !client.connected()) return false;
    size_t written = client.write(p + at, std::min(n - at, size_t(676)));
    if (written > n - at) return false;
    at += written;
    if (at < n) vTaskDelay(1);
  }
  return true;
}
LiveSocket::LiveSocket(uint8_t *storage, size_t capacity, StopFn stop, void *context)
  : parser_(storage, capacity), stop_(stop), context_(context) {}
bool LiveSocket::connect(const char *rawKey) {
  if (!rawKey) { error_ = "Chave Gemini ausente ou inválida."; return false; }
  while (*rawKey == ' ' || *rawKey == '\t') ++rawKey;
  size_t keyLen = strlen(rawKey);
  while (keyLen && (rawKey[keyLen - 1] == ' ' || rawKey[keyLen - 1] == '\t' ||
                    rawKey[keyLen - 1] == '\r' || rawKey[keyLen - 1] == '\n')) --keyLen;
  if (!keyLen || keyLen > 127) { error_ = "Chave Gemini ausente ou inválida."; return false; }
  std::string cleanKey(rawKey, keyLen);
  const char *key = cleanKey.c_str();
  for (const char *p = key; *p; ++p) if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) { error_ = "Chave Gemini inválida."; return false; }
  if (stop_(context_)) return false;
  client_.setInsecure(); // Existing direct TLS policy; PhoneRelay verifies TLS in the app.
  client_.setTimeout(10); // Core 2.0.17: seconds; also sets 10s BLE write credit timeout.
  client_.setHandshakeTimeout(10);
  if (!client_.connect("generativelanguage.googleapis.com", 443, 10000) || stop_(context_)) {
    error_ = "Não foi possível conectar ao Gemini por Wi-Fi ou celular."; return false;
  }
  uint8_t nonce[16]; esp_fill_random(nonce, sizeof(nonce));
  std::string wsKey = base64(nonce, sizeof(nonce));
  std::string challenge = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  if (mbedtls_sha1_ret(reinterpret_cast<const unsigned char *>(challenge.data()), challenge.size(), digest)) {
    error_ = "Falha ao verificar o handshake WebSocket."; return false;
  }
  std::string request = "GET /ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
  request += key;
  request += " HTTP/1.1\r\nHost: generativelanguage.googleapis.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ";
  request += wsKey + "\r\n\r\n";
  if (!writeBytes(client_, reinterpret_cast<const uint8_t *>(request.data()), request.size(), stop_, context_, 5000)) return false;
  request.assign(request.size(), '\0'); request.clear();
  std::string headers; headers.reserve(1024); const uint32_t begin = millis();
  while (!stop_(context_) && !expired(begin, 10000) && headers.size() < 8192) {
    if (client_.available()) {
      int c = client_.read(); if (c < 0) continue;
      headers += char(c);
      if (headers.size() >= 4 && headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
        if (!wsUpgradeValid(headers, base64(digest, sizeof(digest)))) {
          error_ = "Gemini recusou o handshake WebSocket ou enviou resposta inválida."; return false;
        }
        upgraded_ = true; return true;
      }
    } else {
      if (!client_.connected()) break;
      vTaskDelay(1);
    }
  }
  error_ = "Timeout ou cabeçalho WebSocket acima do limite."; return false;
}
bool LiveSocket::frame(uint8_t opcode, const uint8_t *data, size_t n, uint32_t timeout) {
  if (!upgraded_ || closeSent_ || n > kJsonLimit || (opcode >= 8 && n > 125)) return false;
  uint8_t mask[4], header[14], chunk[676]; esp_fill_random(mask, sizeof(mask));
  size_t hn = wsClientHeader(header, opcode, n, mask);
  const uint32_t begin = millis();
  size_t firstPayload = std::min(n, sizeof(chunk) - hn);
  memcpy(chunk, header, hn);
  if (firstPayload) {
    memcpy(chunk + hn, data, firstPayload);
    wsMask(chunk + hn, firstPayload, mask, 0);
  }
  if (!writeBytes(client_, chunk, hn + firstPayload, stop_, context_, timeout)) return false;
  for (size_t at = firstPayload; at < n;) {
    if (expired(begin, timeout)) return false;
    size_t take = std::min(n - at, sizeof(chunk));
    memcpy(chunk, data + at, take); wsMask(chunk, take, mask, at);
    const uint32_t elapsed = uint32_t(millis() - begin);
    if (elapsed >= timeout) return false;
    uint32_t left = timeout - elapsed;
    if (!writeBytes(client_, chunk, take, stop_, context_, left)) return false;
    at += take;
  }
  if (opcode == 8) closeSent_ = true;
  return true;
}
bool LiveSocket::send(const std::string &json) {
  if (json.empty() || !frame(1, reinterpret_cast<const uint8_t *>(json.data()), json.size())) {
    error_ = "Falha ou timeout ao enviar mensagem ao Gemini."; return false;
  }
  return true;
}
WsParser::Event LiveSocket::poll() {
  using E = WsParser::Event;
  if (framePending_ && expired(frameStarted_, 15000)) {
    closeCode_ = 1002; error_ = "Timeout ao receber frame WebSocket."; return E::Error;
  }
  for (size_t i = 0; i < 4096 && client_.available(); ++i) {
    if (stop_(context_)) return E::None;
    int c = client_.read(); if (c < 0) break;
    if (!framePending_) { frameStarted_ = millis(); framePending_ = true; }
    E event = parser_.feed(static_cast<uint8_t>(c));
    if (!parser_.pending()) framePending_ = false;
    if (event == E::Error) {
      error_ = parser_.error();
      closeCode_ = strstr(error_, "UTF8") ? 1007 : strstr(error_, "LIMIT") ? 1009 : 1002;
      return event;
    }
    if (event == E::Ping) {
      if (!frame(10, parser_.data(), parser_.size())) { error_ = "Falha ao responder ping WebSocket."; return E::Error; }
    } else if (event == E::Close) {
      frame(8, parser_.data(), parser_.size(), 300);
      error_ = "Gemini encerrou a conexão antes de concluir a resposta."; return E::Close;
    } else if (event == E::Message) return event;
  }
  if (!client_.connected() && !client_.available()) {
    error_ = parser_.finish() ? "Conexão Gemini interrompida." : parser_.error(); return E::Error;
  }
  return E::None;
}
void LiveSocket::close() {
  if (upgraded_ && !closeSent_ && !stop_(context_) && client_.connected()) {
    const uint8_t code[] = {uint8_t(closeCode_ >> 8), uint8_t(closeCode_)};
    frame(8, code, sizeof(code), 300);
  }
  client_.stop(); upgraded_ = false;
}
std::string ToolGateway::execute(const std::string &url, const std::string &payload,
                                 const std::string &id, StopFn stop, void *ctx, uint32_t budgetMs) {
  std::string path;
  if (!gatewayOrigin(url, path) || payload.empty() || payload.size() > kGatewayLimit)
    return toolError("GATEWAY_CONFIG", "Gateway HTTPS não configurado ou pedido acima do limite.");
  Json request = parseJson(payload.data(), payload.size(), kGatewayLimit);
  if (!request || id != stringField(request.get(), "id"))
    return toolError("GATEWAY_CONFIG", "Pedido do gateway inválido.");
  const std::string token = stringField(request.get(), "token");
  const uint32_t begin = millis(); bool posted = false;
  const uint32_t budget = std::min(uint32_t(25000), budgetMs);
  auto remaining = [&]() -> uint32_t {
    const uint32_t elapsed = uint32_t(millis() - begin);
    return elapsed < budget ? budget - elapsed : 0;
  };
  for (unsigned redirects = 0; redirects <= 3; ++redirects) {
    if (stop(ctx) || remaining() < 2000) return toolError(posted ? "OUTCOME_UNKNOWN" : "CANCELLED",
      posted ? "Pedido enviado; resultado não confirmado. Reconcilie com o mesmo id antes de repetir." : "Ferramenta cancelada antes do envio.", posted);
    NetworkClient client;
    client.setInsecure(); client.setTimeout(1);
    const char *host = redirects ? "script.googleusercontent.com" : "script.google.com";
    // Core 2.0.17 has distinct TCP and TLS connect budgets; split the remaining
    // allowance instead of letting either phase overrun the whole turn.
    const uint32_t connectMs = std::min(uint32_t(5000), remaining() / 2);
    if (connectMs < 1000) return toolError(posted ? "OUTCOME_UNKNOWN" : "CANCELLED", "Prazo do gateway esgotado.", posted);
    client.setHandshakeTimeout(std::max(uint32_t(1), connectMs / 1000));
    if (!client.connect(host, 443, connectMs)) {
      client.stop(); return toolError(posted ? "OUTCOME_UNKNOWN" : "GATEWAY_CONNECT", "Falha de conexão com o gateway.", posted);
    }
    std::string header = std::string(redirects ? "GET " : "POST ") + path + " HTTP/1.1\r\nHost: " + host +
      "\r\nConnection: close\r\nAccept: application/json\r\n";
    if (!redirects) header += "Content-Type: application/json; charset=utf-8\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n";
    header += "\r\n";
    uint32_t left = remaining();
    bool written = left > 1000 && writeBytes(client, reinterpret_cast<const uint8_t *>(header.data()), header.size(),
      stop, ctx, std::min(uint32_t(3000), left - 1000));
    if (written && !redirects) {
      // From here onward even a partial body has an uncertain remote outcome.
      left = remaining();
      if (!stop(ctx) && left > 1000) posted = true;
      written = posted && left > 1000 && writeBytes(client, reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
        stop, ctx, std::min(uint32_t(5000), left - 1000));
    }
    HttpResponse response;
    left = remaining();
    bool read = written && left > 1000 && HttpClient::readResponse(client, response, kGatewayLimit,
      std::min(uint32_t(8000), left - 1000));
    client.stop();
    if (!read || !response.headersComplete || !response.bodyComplete || response.bodyTruncated)
      return toolError(posted ? "OUTCOME_UNKNOWN" : "GATEWAY_IO", "Resposta incompleta do gateway; ação não confirmada.", posted);
    if (response.statusCode == 302 || response.statusCode == 303) {
      const std::string location(response.location.c_str(), response.location.length());
      std::string decoded;
      for (size_t i = 0; i < location.size(); ++i) {
        auto hex = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        if (location[i] == '%' && i + 2 < location.size() && hex(location[i + 1]) >= 0 && hex(location[i + 2]) >= 0) {
          decoded += char(hex(location[i + 1]) * 16 + hex(location[i + 2])); i += 2;
        } else decoded += location[i];
      }
      if (redirects == 3 || !gatewayRedirect(location, path) || (!token.empty() && decoded.find(token) != std::string::npos))
        return toolError("UNSAFE_REDIRECT", "Redirecionamento do gateway recusado; ação pode ter sido executada.", posted);
      continue;
    }
    if (response.statusCode != 200) return toolError("GATEWAY_HTTP", "Gateway retornou erro HTTP; ação não confirmada.", posted);
    Json result = parseJson(response.body.c_str(), response.body.length(), kGatewayLimit);
    const bool success = trueField(result.get(), "ok");
    const bool anonymousError = !success && cJSON_IsNull(field(result.get(), "id"));
    if (!result || !cJSON_IsBool(field(result.get(), "ok")) ||
        cJSON_GetNumberValue(field(result.get(), "version")) != 1 ||
        (id != stringField(result.get(), "id") && !anonymousError) ||
        (success && !field(result.get(), "result")) ||
        (!success && (!cJSON_IsObject(field(result.get(), "error")) || !*stringField(field(result.get(), "error"), "code"))))
      return toolError("GATEWAY_JSON", "Resposta inválida ou id divergente do gateway; ação não confirmada.", posted);
    return printJson(result.get());
  }
  return toolError("REDIRECT_LIMIT", "Limite de redirecionamentos do gateway.", posted);
}
} // namespace Assistant
