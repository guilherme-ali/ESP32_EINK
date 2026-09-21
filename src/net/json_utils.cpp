#include "json_utils.h"
#include <cJSON.h>
#include <ctype.h>
#include <string.h>

namespace {

cJSON *parseDocument(const String &body) {
  if (body.length() == 0) return nullptr;
  return cJSON_Parse(body.c_str());
}

cJSON *pathItem(cJSON *root, const char *path) {
  char copy[192];
  strncpy(copy, path, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';

  cJSON *node = root;
  char *save = nullptr;
  char *part = strtok_r(copy, ".", &save);
  while (part && node) {
    bool numeric = part[0] != '\0';
    for (const char *p = part; *p; p++) {
      if (!isdigit((unsigned char)*p)) {
        numeric = false;
        break;
      }
    }
    node = numeric
               ? cJSON_GetArrayItem(node, atoi(part))
               : cJSON_GetObjectItemCaseSensitive(node, part);
    part = strtok_r(nullptr, ".", &save);
  }
  return node;
}

} // namespace

bool jsonGetString(const String &body, const char *path, char *out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';
  cJSON *root = parseDocument(body);
  if (!root) return false;
  cJSON *item = pathItem(root, path);
  bool ok = item && cJSON_IsString(item) && item->valuestring &&
            item->valuestring[0] != '\0';
  if (ok) {
    strncpy(out, item->valuestring, outLen - 1);
    out[outLen - 1] = '\0';
  }
  cJSON_Delete(root);
  return ok;
}

bool jsonGetLong(const String &body, const char *path, long &out) {
  cJSON *root = parseDocument(body);
  if (!root) return false;
  cJSON *item = pathItem(root, path);
  bool ok = item && cJSON_IsNumber(item);
  if (ok) out = (long)item->valuedouble;
  cJSON_Delete(root);
  return ok;
}
