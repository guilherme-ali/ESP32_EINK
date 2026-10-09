#include "http_client.h"

namespace {

bool readLineUntil(WiFiClientSecure &client, String &line, uint32_t deadline) {
  line = "";
  bool cr = false;
  while ((int32_t)(deadline - millis()) > 0) {
    while (client.available()) {
      int value = client.read();
      if (value < 0) continue;
      if (value == '\n') return cr;
      if (cr || value == 0) return false;
      if (value == '\r') { cr = true; continue; }
      line += (char)value;
      if (line.length() > 4096) return false;
    }
    if (!client.connected() && !client.available()) return false;
    delay(1);
  }
  return false;
}

bool readBytesBounded(WiFiClientSecure &client, size_t count, String &body,
                      size_t maxBody, bool &truncated, uint32_t deadline) {
  size_t remaining = count;
  while (remaining > 0) {
    if ((int32_t)(deadline - millis()) <= 0) return false;
    if (!client.available()) {
      if (!client.connected()) return false;
      delay(1);
      continue;
    }

    uint8_t buffer[512];
    size_t wanted = min(remaining, min(sizeof(buffer), static_cast<size_t>(client.available())));
    int n = client.read(buffer, wanted);
    if (n <= 0) continue;
    if (static_cast<size_t>(n) > wanted) return false;
    for (int i = 0; i < n; ++i) {
      if (body.length() < maxBody) body += static_cast<char>(buffer[i]);
      else truncated = true;
    }
    remaining -= static_cast<size_t>(n);
  }
  return true;
}

uint32_t parseUnsignedHeader(const String &value) {
  String trimmed = value;
  trimmed.trim();
  if (trimmed.length() == 0) return 0;
  uint32_t result = 0;
  for (size_t i = 0; i < trimmed.length(); ++i) {
    if (trimmed[i] < '0' || trimmed[i] > '9') return 0;
    unsigned digit = trimmed[i] - '0';
    if (result > (UINT32_MAX - digit) / 10) return UINT32_MAX;
    result = result * 10 + digit;
  }
  return result;
}

bool unsignedSize(const String &value, unsigned base, size_t &result) {
  if (value.isEmpty()) return false;
  result = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    unsigned digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return false;
    if (digit >= base || result > (SIZE_MAX - digit) / base) return false;
    result = result * base + digit;
  }
  return true;
}

bool connectionTokens(String value, bool &close, bool &keepAlive) {
  if (value.isEmpty()) return false;
  value.toLowerCase();
  while (!value.isEmpty()) {
    int comma = value.indexOf(',');
    String token = comma < 0 ? value : value.substring(0, comma);
    token.trim();
    if (token.isEmpty()) return false;
    for (size_t i = 0; i < token.length(); ++i) {
      char c = token[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    if (token == "close") close = true;
    if (token == "keep-alive") keepAlive = true;
    if (comma < 0) break;
    value = value.substring(comma + 1);
    if (value.isEmpty()) return false;
  }
  return true;
}

} // namespace

bool HttpClient::writeAll(WiFiClientSecure &client, const uint8_t *data, size_t len,
                          uint32_t timeoutMs, WriteProgressFn progress, void *context,
                          bool stopOnResponse) {
  size_t sent = 0;
  uint32_t lastProgress = millis();
  while (sent < len) {
    if (stopOnResponse && client.available()) return false;
    size_t wanted = min(len - sent, static_cast<size_t>(4096));
    size_t written = client.write(data + sent, wanted);
    if (written > wanted) return false;
    if (written > 0) {
      sent += written;
      lastProgress = millis();
      if (progress) progress(written, context);
      continue;
    }
    if (!client.connected() || millis() - lastProgress >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

bool HttpClient::writeAll(WiFiClientSecure &client, const String &data,
                          uint32_t timeoutMs) {
  return writeAll(client, (const uint8_t *)data.c_str(), data.length(), timeoutMs);
}

bool HttpClient::writeFileChunk(WiFiClientSecure &client, File &file, size_t offset,
                                size_t length, uint32_t timeoutMs,
                                WriteProgressFn progress, void *context) {
  if (offset > file.size() || length > file.size() - offset || !file.seek(offset)) return false;
  uint8_t buf[1024];
  size_t remaining = length;
  while (remaining > 0) {
    // Um erro antecipado do servidor precisa ser lido, nao abafado por write().
    if (client.available()) return false;
    size_t toRead = min(remaining, sizeof(buf));
    size_t n = file.read(buf, toRead);
    if (n == 0 || n > toRead) return false;
    if (!writeAll(client, buf, n, timeoutMs, progress, context, true)) return false;
    remaining -= n;
  }
  return true;
}

bool HttpClient::readResponse(WiFiClientSecure &client, HttpResponse &out,
                              size_t maxBody, uint32_t timeoutMs) {
  out = HttpResponse();
  out.body.reserve(min(maxBody, (size_t)2048));
  uint32_t deadline = millis() + timeoutMs;

  String line;
  bool close = false, keepAlive = false, http11 = false;
  unsigned informational = 0;
  size_t headerBytes = 0;
  do {
    if (!readLineUntil(client, line, deadline) ||
        !(line.startsWith("HTTP/1.1 ") || line.startsWith("HTTP/1.0 ")) ||
        line.length() < 12 || (line.length() > 12 && line[12] != ' ')) {
      client.stop(); return false;
    }
    http11 = line.startsWith("HTTP/1.1 ");
    size_t status;
    if (!unsignedSize(line.substring(9, 12), 10, status) || status < 100 || status > 599 || status == 101) {
      client.stop(); return false;
    }
    out.statusCode = static_cast<int>(status);
    out.hasContentLength = false;
    out.contentLength = 0;
    out.chunked = false;
    out.location = "";
    out.range = "";
    out.retryAfterSec = 0;
    close = keepAlive = false;
    while (true) {
      if (!readLineUntil(client, line, deadline)) {
        client.stop();
        return false;
      }
      headerBytes += line.length() + 2;
      if (headerBytes > 16384) { client.stop(); return false; }
      if (line.length() == 0) break;

      int colon = line.indexOf(':');
      if (colon <= 0) { client.stop(); return false; }
      String name = line.substring(0, colon);
      String value = line.substring(colon + 1);
      name.toLowerCase();
      value.trim();

      if (name == "content-length") {
        size_t length;
        if (!unsignedSize(value, 10, length) ||
            (out.hasContentLength && out.contentLength != length)) { client.stop(); return false; }
        out.hasContentLength = true;
        out.contentLength = length;
      } else if (name == "transfer-encoding") {
        value.toLowerCase();
        if (out.chunked || value != "chunked") { client.stop(); return false; }
        out.chunked = true;
      } else if (name == "connection") {
        if (!connectionTokens(value, close, keepAlive)) { client.stop(); return false; }
      } else if (name == "location") {
        if (!out.location.isEmpty()) { client.stop(); return false; }
        out.location = value;
      } else if (name == "range") {
        if (!out.range.isEmpty()) { client.stop(); return false; }
        out.range = value;
      } else if (name == "retry-after") {
        out.retryAfterSec = parseUnsignedHeader(value);
      }
    }
    if (out.chunked && out.hasContentLength) { client.stop(); return false; }
    if (out.statusCode < 200 && ++informational > 4) { client.stop(); return false; }
  } while (out.statusCode < 200);
  out.headersComplete = true;
  bool noBody = out.statusCode == 204 || out.statusCode == 304;
  // HTTP/1.1 e persistente por padrao, mas so com framing inequivoco.
  bool connectionClose = close || (!http11 && !keepAlive) ||
                         !(noBody || out.hasContentLength || out.chunked);
  if (out.statusCode == 204 && (out.chunked || (out.hasContentLength && out.contentLength))) {
    client.stop(); return false;
  }

  if (out.hasContentLength && out.contentLength == 0) {
    out.bodyComplete = true;
    out.connectionClose = connectionClose;
    return true;
  }
  if (noBody) {
    out.bodyComplete = true;
    out.connectionClose = connectionClose;
    return true;
  }

  if (out.chunked) {
    while (true) {
      if (!readLineUntil(client, line, deadline)) {
        client.stop();
        return false;
      }
      line.trim();
      int semicolon = line.indexOf(';');
      if (semicolon >= 0) line = line.substring(0, semicolon);
      size_t chunkSize;
      if (!unsignedSize(line, 16, chunkSize)) {
        client.stop();
        return false;
      }
      if (chunkSize == 0) {
        // Trailer headers, if any, are irrelevant to our bounded JSON body.
        do {
          if (!readLineUntil(client, line, deadline)) { client.stop(); return false; }
          headerBytes += line.length() + 2;
          if (headerBytes > 16384 || (!line.isEmpty() && line.indexOf(':') <= 0)) {
            client.stop(); return false;
          }
        } while (line.length() > 0);
        out.bodyComplete = true;
        out.connectionClose = connectionClose;
        return true;
      }
      if (!readBytesBounded(client, chunkSize, out.body, maxBody,
                            out.bodyTruncated, deadline)) {
        client.stop();
        return false;
      }
      if (!readLineUntil(client, line, deadline) || line.length() != 0) {
        client.stop();
        return false;
      }
    }
  }

  if (out.hasContentLength) {
    out.bodyComplete = readBytesBounded(client, out.contentLength, out.body,
                                         maxBody, out.bodyTruncated, deadline);
    if (!out.bodyComplete) client.stop();
    else out.connectionClose = connectionClose;
    return out.bodyComplete;
  }

  // APIs should send Content-Length or chunked encoding. For a legacy
  // close-delimited response, accept only when the peer really closes.
  while (client.connected() || client.available()) {
    if ((int32_t)(deadline - millis()) <= 0) {
      client.stop();
      return false;
    }
    if (!client.available()) {
      delay(1);
      continue;
    }
    int value = client.read();
    if (value >= 0) {
      if (out.body.length() < maxBody) out.body += (char)value;
      else out.bodyTruncated = true;
    }
  }
  out.bodyComplete = true;
  return true;
}

bool HttpClient::parseHttpsUrl(const String &url, String &host, uint16_t &port,
                               String &path) {
  if (!url.startsWith("https://")) return false;
  String rest = url.substring(8);
  int slash = rest.indexOf('/');
  String hostPort = slash >= 0 ? rest.substring(0, slash) : rest;
  path = slash >= 0 ? rest.substring(slash) : "/";
  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = (uint16_t)hostPort.substring(colon + 1).toInt();
  } else {
    host = hostPort;
    port = 443;
  }
  return host.length() > 0 && port > 0;
}

bool HttpClient::isRetryableStatus(int statusCode, const String &body) {
  if (statusCode == 0 || statusCode == 408 || statusCode == 429 ||
      statusCode == 500 || statusCode == 502 || statusCode == 503 ||
      statusCode == 504) {
    return true;
  }
  if (statusCode == 403 &&
      (body.indexOf("rateLimitExceeded") >= 0 ||
       body.indexOf("userRateLimitExceeded") >= 0 ||
       body.indexOf("backendError") >= 0)) {
    return true;
  }
  return false;
}

uint32_t HttpClient::retryDelayMs(int attempt, uint32_t retryAfterSec) {
  if (retryAfterSec > 60) return UINT32_MAX;
  if (retryAfterSec > 0) return retryAfterSec * 1000UL;
  int boundedAttempt = constrain(attempt, 0, 5);
  uint32_t base = 1000UL << boundedAttempt;
  return base + (uint32_t)random(0, 251);
}
