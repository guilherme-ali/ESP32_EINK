#include "http_client.h"

namespace {

bool readLineUntil(WiFiClientSecure &client, String &line, uint32_t deadline) {
  line = "";
  while ((int32_t)(deadline - millis()) > 0) {
    while (client.available()) {
      int value = client.read();
      if (value < 0) continue;
      if (value == '\r') continue;
      if (value == '\n') return true;
      line += (char)value;
      if (line.length() > 512) return false;
    }
    if (!client.connected() && !client.available()) return line.length() > 0;
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

    int value = client.read();
    if (value < 0) continue;
    if (body.length() < maxBody) {
      body += (char)value;
    } else {
      truncated = true;
    }
    remaining--;
  }
  return true;
}

uint32_t parseUnsignedHeader(const String &value) {
  String trimmed = value;
  trimmed.trim();
  if (trimmed.length() == 0) return 0;
  return (uint32_t)strtoul(trimmed.c_str(), nullptr, 10);
}

} // namespace

bool HttpClient::writeAll(WiFiClientSecure &client, const uint8_t *data, size_t len,
                          uint32_t timeoutMs) {
  size_t sent = 0;
  uint32_t lastProgress = millis();
  while (sent < len) {
    size_t written = client.write(data + sent, len - sent);
    if (written > 0) {
      sent += written;
      lastProgress = millis();
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
                                size_t length, uint32_t timeoutMs) {
  if (!file.seek(offset)) return false;
  uint8_t buf[1024];
  size_t remaining = length;
  while (remaining > 0) {
    size_t toRead = min(remaining, sizeof(buf));
    size_t n = file.read(buf, toRead);
    if (n == 0) return false;
    if (!writeAll(client, buf, n, timeoutMs)) return false;
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
  if (!readLineUntil(client, line, deadline)) {
    client.stop();
    return false;
  }
  int firstSpace = line.indexOf(' ');
  if (firstSpace < 0 || firstSpace + 4 > (int)line.length()) {
    client.stop();
    return false;
  }
  out.statusCode = line.substring(firstSpace + 1, firstSpace + 4).toInt();

  while (true) {
    if (!readLineUntil(client, line, deadline)) {
      client.stop();
      return false;
    }
    if (line.length() == 0) break;

    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon);
    String value = line.substring(colon + 1);
    name.toLowerCase();
    value.trim();

    if (name == "content-length") {
      out.hasContentLength = true;
      out.contentLength = (size_t)strtoull(value.c_str(), nullptr, 10);
    } else if (name == "transfer-encoding" && value.indexOf("chunked") >= 0) {
      out.chunked = true;
    } else if (name == "location") {
      out.location = value;
    } else if (name == "range") {
      out.range = value;
    } else if (name == "retry-after") {
      out.retryAfterSec = parseUnsignedHeader(value);
    }
  }
  out.headersComplete = true;

  if (out.hasContentLength && out.contentLength == 0) {
    out.bodyComplete = true;
    return true;
  }

  if (out.chunked) {
    while (true) {
      if (!readLineUntil(client, line, deadline)) {
        client.stop();
        return false;
      }
      line.trim();
      if (line.length() == 0) continue;
      size_t semicolon = line.indexOf(';');
      if (semicolon >= 0) line = line.substring(0, semicolon);
      long chunkSize = strtol(line.c_str(), nullptr, 16);
      if (chunkSize < 0) {
        client.stop();
        return false;
      }
      if (chunkSize == 0) {
        // Trailer headers, if any, are irrelevant to our bounded JSON body.
        while (readLineUntil(client, line, deadline) && line.length() > 0) {
        }
        out.bodyComplete = true;
        return true;
      }
      if (!readBytesBounded(client, (size_t)chunkSize, out.body, maxBody,
                            out.bodyTruncated, deadline)) {
        client.stop();
        return false;
      }
      if (!readLineUntil(client, line, deadline)) {
        client.stop();
        return false;
      }
    }
  }

  if (out.hasContentLength) {
    out.bodyComplete = readBytesBounded(client, out.contentLength, out.body,
                                        maxBody, out.bodyTruncated, deadline);
    if (!out.bodyComplete) client.stop();
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
  if (retryAfterSec > 60) return 0;
  if (retryAfterSec > 0) return retryAfterSec * 1000UL;
  int boundedAttempt = constrain(attempt, 0, 5);
  uint32_t base = 1000UL << boundedAttempt;
  return base + (uint32_t)random(0, 251);
}
