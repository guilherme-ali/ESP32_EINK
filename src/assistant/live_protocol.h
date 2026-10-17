#pragma once
#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>
#include <cJSON.h>

namespace Assistant {
constexpr size_t kJsonLimit = 256 * 1024;
constexpr size_t kGatewayLimit = 32 * 1024;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parseJson(const char *data, size_t length, size_t limit = kJsonLimit);
std::string printJson(const cJSON *json);
const cJSON *field(const cJSON *json, const char *key);
const char *stringField(const cJSON *json, const char *key);
bool trueField(const cJSON *json, const char *key);
bool allowedModel(const char *model, bool freeConfirmed);
std::string setupJson(const char *model, bool freeConfirmed, const char *timeZone,
                      int64_t utcEpoch, bool toolsEnabled);
std::string activityJson(bool start);
std::string audioJson(const uint8_t *pcm, size_t length);
std::string toolResponseJson(const std::string &id, const std::string &name, const std::string &result);
std::string toolError(const char *code, const char *message, bool uncertain = false);
std::string gatewayRequest(const std::string &token, const std::string &id,
                           const std::string &tool, const std::string &args);
std::string gatewayId(uint64_t nonce, unsigned index);
bool knownTool(const std::string &name);
bool gatewayOrigin(const std::string &url, std::string &path);
bool gatewayRedirect(const std::string &url, std::string &path);
std::string base64(const uint8_t *data, size_t length);
bool unbase64(const char *data, size_t length, std::vector<uint8_t> &out, size_t limit);
std::string redact(std::string text, const std::string &apiKey, const std::string &gatewayToken);
} // namespace Assistant
