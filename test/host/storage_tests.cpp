#include "fixtures.h"
#include "gdrive.h"
#include "../../src/storage/note_files.h"
#include <LittleFS.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
struct Test { const char *name; std::function<void()> run; };
std::vector<Test> &tests() { static std::vector<Test> all; return all; }
struct Register { Register(const char *name, std::function<void()> run) { tests().push_back({name, run}); } };
#define TEST(name) void name(); Register register_##name(#name, name); void name()
#define CHECK(c) do { if (!(c)) throw std::runtime_error(std::string("linha ") + std::to_string(__LINE__) + ": " + #c); } while (false)
constexpr const char *wav = "/notes/test.wav", *sync = "/notes/test.sync", *tmp = "/notes/test.sync.tmp";
void seedString(const char *key, const char *value) { Host::seedNvs(key, std::string(value)); }
std::string nvsString(const char *key) { return std::get<std::string>(Host::nvs.at("cfg").at(key)); }
SettingsStore settings() {
  SettingsStore s; CHECK(s.begin()); return s;
}
NoteSyncState uploaded(bool text = false) {
  auto s = settings();
  LittleFS.put(wav, Fixtures::wav());
  NoteSyncState state;
  state.authGeneration = s.get().driveAuthGeneration;
  state.wavUploaded = state.fullySynced = true;
  std::strcpy(state.folderId, "folder"); std::strcpy(state.wavDriveId, "old_wav");
  std::strcpy(state.wavMd5, Host::mockFileHash(Fixtures::wav()).c_str());
  state.wavSize = state.wavBytesUploaded = Fixtures::wav().size();
  if (text) {
    LittleFS.put("/notes/test.txt", "texto");
    state.txtUploaded = true; std::strcpy(state.txtDriveId, "old_txt");
    std::strcpy(state.txtMd5, Host::mockFileHash("texto").c_str());
    state.txtSize = state.txtBytesUploaded = 5;
  }
  CHECK(state.save(wav)); return state;
}
NoteSyncState load() { NoteSyncState s; CHECK(s.load(wav)); return s; }
void reply(const std::string &body, int status = 200, const std::string &headers = "") {
  Host::enqueue(Fixtures::http(body, status, headers), 3, 1);
}
void oauth() { reply(R"({"access_token":"access","token_type":"Bearer"})"); }
void folder() { reply(R"({"id":"folder","mimeType":"application/vnd.google-apps.folder","trashed":false})"); }
std::string remote(const char *id, const std::string &bytes, const String &hash) {
  return std::string("{\"id\":\"") + id + "\",\"name\":\"test.wav\",\"trashed\":false,\"parents\":[\"folder\"],\"size\":\"" +
         std::to_string(bytes.size()) + "\",\"md5Checksum\":\"" + hash.c_str() + "\"}";
}
SettingsStore driveSettings() {
  auto s = settings(); CHECK(s.saveDriveApp("client", "secret"));
  CHECK(s.saveDriveRefreshToken("refresh")); CHECK(s.saveDriveFolderId("folder")); return s;
}
void newUploadReplies(const std::string &bytes, bool badHash = false) {
  oauth(); folder(); reply(R"({"files":[]})"); reply(R"({"ids":["new_wav"]})");
  reply("{}", 404); reply("{}", 200, "Location: https://www.googleapis.com/upload/session\r\n");
  reply("{}", 200);
  reply(remote("new_wav", bytes, badHash ? Host::mockFileHash("different") : Host::mockFileHash(bytes)));
}

TEST(stubs_shared_handles_atomic_replacement_and_namespace_isolation) {
  File writer = LittleFS.open("/a", FILE_WRITE), reader = LittleFS.open("/a");
  CHECK(writer.print("abc") == 3); writer.flush();
  CHECK(reader.size() == 3 && reader.readString() == "abc" && LittleFS.exists("/a"));
  LittleFS.put("/b", "old"); LittleFS.faults->failRename = "/a";
  CHECK(!LittleFS.rename("/a", "/b") && LittleFS.bytes("/b") == "old");
  LittleFS.faults->failRename.clear();
  CHECK(!LittleFS.rename("/a", "/b")); // LittleFS recusa arquivo aberto.
  writer.close(); reader.close();
  CHECK(LittleFS.rename("/a", "/b"));
  CHECK(!LittleFS.exists("/a") && LittleFS.bytes("/b") == "abc" && LittleFS.faults->flushes == 1);
  Preferences a, b; CHECK(a.begin("cfg") && b.begin("other"));
  CHECK(a.putString("key", "abc") == 3 && !b.isKey("key"));
  CHECK(a.putString("empty", "") == 0 && a.isKey("empty"));
  Host::failPuts.insert("cfg/key"); CHECK(a.putString("key", "new") == 0 && a.getString("key") == "abc");
}
TEST(mock_hash_known_vector_and_chunk_invariance_not_crypto) {
  CHECK(Host::mockFileHash("abc") == "1a47e90b1a47e90b1a47e90b1a47e90b");
  MD5Builder h; h.begin(); uint8_t a[] = {'a'}, bc[] = {'b', 'c'};
  h.add(a, 1); h.add(bc, 2); h.calculate(); CHECK(h.toString() == Host::mockFileHash("abc"));
  CHECK(Host::mockFileHash("abc") != Host::mockFileHash("abd"));
}
TEST(state_v3_roundtrip_long_sessions_reserved_and_uploaded_distinct) {
  NoteSyncState s; s.authGeneration = 37; s.lastStatusCode = 308; s.lastAttemptTime = UINT32_MAX;
  std::strcpy(s.folderId, "folder"); std::strcpy(s.pendingFolderId, "pending");
  std::strcpy(s.lastError, "retry pending");
  std::strcpy(s.wavDriveId, std::string(127, 'w').c_str()); s.wavIdReserved = true;
  s.wavSessionUrl = String("https://www.googleapis.com/upload/") + String(std::string(3500, 's'));
  s.wavSize = 60; s.wavBytesUploaded = 13; std::strcpy(s.wavMd5, Host::mockFileHash("wav").c_str());
  s.txtUploaded = true; std::strcpy(s.txtDriveId, "txt");
  s.txtSize = s.txtBytesUploaded = 5; std::strcpy(s.txtMd5, Host::mockFileHash("texto").c_str());
  s.mdIdReserved = true; std::strcpy(s.mdDriveId, "md");
  s.mdSessionUrl = "https://content.googleapis.com/upload/md"; s.mdSize = 9; s.mdBytesUploaded = 2;
  std::strcpy(s.mdMd5, Host::mockFileHash("markdown").c_str());
  CHECK(s.save(wav)); const auto raw = LittleFS.bytes(sync); auto actual = load(); CHECK(actual.save(wav));
  CHECK(LittleFS.bytes(sync) == raw && actual.version == 3 && actual.authGeneration == 37);
  CHECK(actual.wavSessionUrl == s.wavSessionUrl && actual.wavIdReserved && !actual.wavUploaded);
  CHECK(actual.txtUploaded && !actual.txtIdReserved && !actual.fullySynced && !LittleFS.exists(tmp));
}
TEST(state_valid_temp_supersedes_old_and_rename_failure_retries) {
  NoteSyncState s; s.authGeneration = 1; CHECK(s.save(wav)); const auto old = LittleFS.bytes(sync);
  s.authGeneration = 2; CHECK(s.save(wav)); const auto recent = LittleFS.bytes(sync);
  LittleFS.put(sync, old); LittleFS.put(tmp, recent); LittleFS.faults->failRename = tmp;
  NoteSyncState failed; CHECK(!failed.load(wav) && failed.lastError[0]);
  CHECK(LittleFS.bytes(sync) == old && LittleFS.bytes(tmp) == recent);
  LittleFS.faults->failRename.clear(); CHECK(load().authGeneration == 2);
  CHECK(LittleFS.bytes(sync) == recent && !LittleFS.exists(tmp));
}
TEST(state_partial_write_and_corrupt_flush_keep_old) {
  NoteSyncState s; s.authGeneration = 1; CHECK(s.save(wav)); const auto old = LittleFS.bytes(sync);
  s.authGeneration = 2; LittleFS.faults->writeLimit = 17;
  CHECK(!s.save(wav) && LittleFS.bytes(sync) == old && LittleFS.exists(tmp));
  CHECK(load().authGeneration == 1);
  LittleFS.faults->writeLimit = SIZE_MAX; LittleFS.faults->corruptFlush = tmp;
  CHECK(!s.save(wav) && LittleFS.bytes(sync) == old && load().authGeneration == 1);
}
TEST(state_corrupt_unknown_schema_is_not_sync_proof) {
  for (const char *bad : {"{", R"({"version":4})", R"({"version":3,"authGeneration":-1})", "{}{}"}) {
    LittleFS.clear(); LittleFS.put(sync, bad); NoteSyncState s;
    CHECK(!s.load(wav) && s.lastError[0] && !s.fullySynced && LittleFS.bytes(sync) == bad);
  }
  LittleFS.clear(); LittleFS.put(tmp, "{"); NoteSyncState s;
  CHECK(!s.load(wav) && s.lastError[0] && !LittleFS.exists(sync));
}
TEST(state_legacy_v1_v2_migrate_without_trusting_legacy_flags) {
  LittleFS.put(sync, R"({"version":1,"wavDriveId":"legacy","wavUploaded":true,"fullySynced":true})");
  auto s = load(); CHECK(s.version == 3 && !s.fullySynced && !s.wavUploaded && !s.wavDriveId[0]);
  LittleFS.clear(); LittleFS.put("/notes/test.snc", "legacy");
  CHECK(load().version == 3 && LittleFS.exists(sync));
  s = NoteSyncState(); CHECK(s.save(wav)); std::string v2 = LittleFS.bytes(sync);
  v2.replace(v2.find("\"version\":3"), 11, "\"version\":2"); LittleFS.put(sync, v2);
  CHECK(load().version == 3 && LittleFS.bytes(sync).find("\"version\":3") != std::string::npos);
}
TEST(state_reserved_id_is_not_fully_synced_and_invalid_flags_refused) {
  auto s = settings(); LittleFS.put(wav, Fixtures::wav()); NoteSyncState state;
  state.authGeneration = s.get().driveAuthGeneration; state.wavIdReserved = true;
  std::strcpy(state.wavDriveId, "reserved"); CHECK(state.save(wav));
  CHECK(!load().wavUploaded && GDriveClient::needsUpload(wav, false));
  NoteSyncState visible; CHECK(GDriveClient::getNoteSyncState(wav, visible) && !visible.fullySynced);
  state.fullySynced = true; CHECK(!state.save(wav));
  state.fullySynced = false; state.wavUploaded = true; CHECK(!state.save(wav));
}
TEST(needs_upload_generation_change_and_required_regenerated_text) {
  auto state = uploaded(); CHECK(!GDriveClient::needsUpload(wav, false));
  CHECK(GDriveClient::needsUpload(wav, true)); // TXT/MD regeneration required
  LittleFS.put("/notes/test.txt", "novo texto"); CHECK(GDriveClient::needsUpload(wav, false));
  LittleFS.remove("/notes/test.txt"); Host::rebootNvs(); auto s = settings();
  CHECK(s.saveDriveRefreshToken("new auth") && SettingsStore::currentDriveGeneration() != state.authGeneration);
  CHECK(GDriveClient::needsUpload(wav, false));
  NoteSyncState visible; CHECK(GDriveClient::getNoteSyncState(wav, visible) && !visible.fullySynced);
}
TEST(missing_previously_uploaded_txt_invalidates_get_state) {
  uploaded(true); CHECK(!GDriveClient::needsUpload(wav, false)); LittleFS.remove("/notes/test.txt");
  CHECK(GDriveClient::needsUpload(wav, false)); NoteSyncState visible;
  CHECK(GDriveClient::getNoteSyncState(wav, visible) && !visible.fullySynced && !visible.txtUploaded);
  CHECK(load().txtUploaded); // UI query is read-only
}
TEST(source_change_same_size_keeps_saved_hash_but_invalidates_sync) {
  auto state = uploaded(); const std::string original = Fixtures::wav(); std::string changed = original;
  changed.back() = 1; LittleFS.put(wav, changed);
  CHECK(original.size() == changed.size() && Host::mockFileHash(original) != Host::mockFileHash(changed));
  CHECK(GDriveClient::needsUpload(wav, false)); NoteSyncState visible;
  CHECK(GDriveClient::getNoteSyncState(wav, visible) && !visible.wavUploaded && !visible.fullySynced);
  CHECK(std::string(load().wavMd5) == state.wavMd5); // expected stored hash never rewritten by a query
}

TEST(settings_begin_failure_and_strict_512_513_refresh_token) {
  Host::failNvsBegin = true; SettingsStore failed; CHECK(!failed.begin()); Host::failNvsBegin = false;
  auto s = settings(); std::string accepted(512, 't'), rejected(513, 't');
  CHECK(s.saveDriveRefreshToken(accepted.c_str()) && nvsString("driveRefTok") == accepted);
  uint32_t generation = s.get().driveAuthGeneration;
  CHECK(!s.saveDriveRefreshToken(rejected.c_str()) && std::string(s.get().driveRefreshToken) == accepted);
  CHECK(s.get().driveAuthGeneration == generation && nvsString("driveRefTok") == accepted);
  Host::rebootNvs(); auto reloaded = settings(); CHECK(std::string(reloaded.get().driveRefreshToken) == accepted);
  CHECK(reloaded.saveDriveRefreshToken("") && !reloaded.hasDriveAuth() && nvsString("driveRefTok").empty());
}
TEST(settings_put_failures_return_false_without_changing_target_fields) {
  auto s = settings(); CHECK(s.saveDriveFolderId("old_folder")); Host::failPuts.insert("cfg/driveFolder");
  CHECK(!s.saveDriveFolderId("new_folder") && std::string(s.get().driveFolderId) == "old_folder");
  CHECK(nvsString("driveFolder") == "old_folder"); Host::failPuts.clear();
  CHECK(s.saveAiPolicy(true, "old_summary", true)); Host::failPuts.insert("cfg/summaryModel");
  CHECK(!s.saveAiPolicy(false, "new_summary", false));
  CHECK(s.get().sttAutoModel && s.get().geminiFreeConfirmed && std::string(s.get().summaryModel) == "old_summary");
}
TEST(settings_refresh_put_failure_keeps_ram_generation_and_token) {
  auto s = settings(); CHECK(s.saveDriveRefreshToken("old_token")); const auto generation = s.get().driveAuthGeneration;
  Host::failPuts.insert("cfg/driveRefTok"); CHECK(!s.saveDriveRefreshToken("new_token"));
  CHECK(std::string(s.get().driveRefreshToken) == "old_token" && nvsString("driveRefTok") == "old_token");
  CHECK(s.get().driveAuthGeneration == generation && SettingsStore::currentDriveGeneration() == generation);
}
TEST(settings_stt_put_failure_keeps_ram_policy_and_credentials) {
  auto s = settings(); CHECK(s.saveStt("https://old.test", "old_model", "old_key"));
  CHECK(s.saveAiPolicy(true, "summary", true)); Host::failPuts.insert("cfg/sttApiKey");
  CHECK(!s.saveStt("https://new.test", "new_model", "new_key"));
  CHECK(std::string(s.get().sttEndpoint) == "https://old.test" && std::string(s.get().sttApiKey) == "old_key");
  CHECK(s.get().geminiFreeConfirmed);
}
TEST(settings_drive_app_put_failure_keeps_ram_authorization) {
  auto s = driveSettings(); const auto generation = s.get().driveAuthGeneration;
  Host::failPuts.insert("cfg/driveCliSec"); CHECK(!s.saveDriveApp("new_client", "new_secret"));
  CHECK(std::string(s.get().driveClientId) == "client" && std::string(s.get().driveClientSecret) == "secret");
  CHECK(std::string(s.get().driveRefreshToken) == "refresh" && std::string(s.get().driveFolderId) == "folder");
  CHECK(s.get().driveAuthGeneration == generation);
}
TEST(settings_generation_write_failure_and_success_reload) {
  auto s = settings(); Host::failPuts.insert("cfg/driveAuthGen"); CHECK(!s.saveDriveRefreshToken("token"));
  CHECK(!s.hasDriveAuth() && s.get().driveAuthGeneration == 1 && !Host::nvs["cfg"].count("driveRefTok"));
  Host::failPuts.clear(); CHECK(s.saveDriveRefreshToken("token")); const auto generation = s.get().driveAuthGeneration;
  CHECK(s.saveDriveRefreshToken("token") && s.get().driveAuthGeneration == generation);
  Host::rebootNvs(); auto reloaded = settings(); CHECK(reloaded.get().driveAuthGeneration == generation);
  CHECK(SettingsStore::currentDriveGeneration() == generation);
}
TEST(settings_free_migration_legacy_key_without_confirmation) {
  seedString("sttEndpoint", "https://generativelanguage.googleapis.com/v1beta"); seedString("sttApiKey", "legacy_key");
  auto s = settings(); CHECK(s.get().geminiFreeConfirmed && Host::nvs["cfg"].count("freeConfirmed"));
  CHECK(Host::nvs["cfg"].count("aiPolicyV")); CHECK(s.saveStt(s.get().sttEndpoint, s.get().sttModel, "new_key"));
  CHECK(!s.get().geminiFreeConfirmed); Host::rebootNvs(); CHECK(!settings().get().geminiFreeConfirmed);
}
TEST(settings_free_migration_preserves_current_users_explicit_false) {
  seedString("sttEndpoint", "https://generativelanguage.googleapis.com/v1beta"); seedString("sttApiKey", "current_key");
  Host::seedNvs("freeConfirmed", uint8_t(0)); auto s = settings();
  CHECK(!s.get().geminiFreeConfirmed && std::get<uint8_t>(Host::nvs["cfg"].at("freeConfirmed")) == 0);
}
TEST(settings_free_migration_does_not_confirm_new_or_non_gemini_key) {
  auto s = settings(); CHECK(!s.get().geminiFreeConfirmed);
  CHECK(s.saveStt("https://generativelanguage.googleapis.com/v1beta", "model", "new_key"));
  Host::rebootNvs(); CHECK(!settings().get().geminiFreeConfirmed);
  Host::resetNvs(); seedString("sttEndpoint", "https://other.test"); seedString("sttApiKey", "key");
  CHECK(!settings().get().geminiFreeConfirmed);
}

TEST(note_write_atomic_partial_open_and_rename_failures_preserve_old) {
  const char *path = "/notes/test.txt"; LittleFS.put(path, "old");
  LittleFS.faults->failOpen = "/notes/test.txt.tmp";
  CHECK(!NoteFiles::writeAtomic(path, "new text", 8) && LittleFS.bytes(path) == "old");
  LittleFS.faults->failOpen.clear(); LittleFS.faults->writeLimit = 3;
  CHECK(!NoteFiles::writeAtomic(path, "new text", 8) && LittleFS.bytes(path) == "old");
  CHECK(!LittleFS.exists("/notes/test.txt.tmp")); LittleFS.faults->writeLimit = SIZE_MAX;
  LittleFS.faults->failRename = "/notes/test.txt.tmp";
  CHECK(!NoteFiles::writeAtomic(path, "new text", 8) && LittleFS.bytes(path) == "old");
  CHECK(LittleFS.bytes("/notes/test.txt.tmp") == "new text"); LittleFS.faults->failRename.clear();
  CHECK(NoteFiles::writeAtomic(path, "new text", 8) && LittleFS.bytes(path) == "new text");
}
TEST(note_atomic_replace_requires_closed_destination_littlefs_contract) {
  LittleFS.put("/notes/test.ai", "old metadata");
  File open = NoteFiles::fs().open("/notes/test.ai", FILE_READ);
  CHECK(!NoteFiles::writeAtomic("/notes/test.ai", "new metadata", 12));
  CHECK(LittleFS.bytes("/notes/test.ai") == "old metadata");
  open.close();
  CHECK(NoteFiles::writeAtomic("/notes/test.ai", "new metadata", 12));
  CHECK(LittleFS.bytes("/notes/test.ai") == "new metadata");
}
TEST(note_recovery_prefers_valid_old_then_valid_tmp) {
  LittleFS.put("/notes/test.txt", "old"); LittleFS.put("/notes/test.txt.tmp", "new");
  NoteFiles::recoverText("/notes/test.txt"); CHECK(LittleFS.bytes("/notes/test.txt") == "old" && !LittleFS.exists("/notes/test.txt.tmp"));
  LittleFS.put("/notes/test.txt", " "); LittleFS.put("/notes/test.txt.tmp", "new");
  NoteFiles::recoverText("/notes/test.txt"); CHECK(LittleFS.bytes("/notes/test.txt") == "new");
  LittleFS.remove("/notes/test.txt"); LittleFS.put("/notes/test.txt.tmp", std::string("\xc0\xaf"));
  NoteFiles::recoverText("/notes/test.txt"); CHECK(!LittleFS.exists("/notes/test.txt") && !LittleFS.exists("/notes/test.txt.tmp"));
}
TEST(note_full_flash_reserve_and_atomic_admission_boundary) {
  LittleFS.capacity = NoteFiles::kReserveBytes; CHECK(NoteFiles::recordingBytes() == 0);
  LittleFS.capacity += 42; CHECK(NoteFiles::recordingBytes() == 42);
  LittleFS.put("/notes/test.txt", "old"); LittleFS.capacity = LittleFS.usedBytes() + 8192 + 2;
  CHECK(!NoteFiles::writeAtomic("/notes/test.txt", "new", 3) && LittleFS.bytes("/notes/test.txt") == "old");
  CHECK(!LittleFS.exists("/notes/test.txt.tmp")); ++LittleFS.capacity;
  CHECK(NoteFiles::writeAtomic("/notes/test.txt", "new", 3));
  LittleFS.capacity = LittleFS.usedBytes(); CHECK(NoteFiles::freeBytes() == 0 && NoteFiles::recordingBytes() == 0);
  CHECK(!NoteFiles::writeAtomic("/notes/test.txt", "more", 4));
}
TEST(note_read_long_utf8_short_reads_and_strict_limits) {
  std::string text; while (text.size() + 5 <= NoteFiles::kMaxTextBytes) text += "ação";
  LittleFS.put("/notes/test.txt", text); LittleFS.faults->readLimit = 7;
  std::vector<char> out(text.size() + 1); CHECK(NoteFiles::readText("/notes/test.txt", out.data(), out.size()));
  CHECK(std::string(out.data()) == text && NoteFiles::validText("/notes/test.txt"));
  CHECK(!NoteFiles::readText("/notes/test.txt", out.data(), text.size()) && out[0] == '\0');
  LittleFS.faults->readBudget = 100; CHECK(!NoteFiles::readText("/notes/test.txt", out.data(), out.size()) && out[0] == '\0');
  LittleFS.faults->readBudget = SIZE_MAX;
  std::string boundary(NoteFiles::kMaxTextBytes, 'x'); LittleFS.put("/notes/test.txt", boundary);
  std::vector<char> exact(boundary.size() + 1);
  CHECK(NoteFiles::readText("/notes/test.txt", exact.data(), exact.size()) && std::string(exact.data()) == boundary);
  CHECK(NoteFiles::writeAtomic("/notes/out.txt", boundary.c_str(), boundary.size()));
  for (const std::string &bad : {std::string("\xe2\x82"), std::string("a\0b", 3), std::string(" \n\t"), std::string(32768, 'x')}) {
    LittleFS.put("/notes/test.txt", bad); std::vector<char> buffer(bad.size() + 1, 'Z');
    CHECK(!NoteFiles::readText("/notes/test.txt", buffer.data(), buffer.size()) && buffer[0] == '\0');
    CHECK(!NoteFiles::writeAtomic("/notes/out.txt", bad.c_str(), bad.size()));
  }
}
TEST(note_valid_wav_requires_canonical_header_and_exact_size) {
  const auto bytes = Fixtures::wav(); LittleFS.put(wav, bytes); CHECK(NoteFiles::validWav(wav));
  for (size_t offset : {size_t(0), size_t(12), size_t(16), size_t(20), size_t(22), size_t(28), size_t(32), size_t(34), size_t(36), size_t(40)}) {
    auto bad = bytes; bad[offset] ^= 1; LittleFS.put(wav, bad); CHECK(!NoteFiles::validWav(wav));
  }
  for (const auto &bad : {bytes.substr(0, 43), bytes.substr(0, bytes.size() - 1), bytes + "x"}) {
    LittleFS.put(wav, bad); CHECK(!NoteFiles::validWav(wav));
  }
}

TEST(drive_generate_id_persist_before_requests_and_final_get_verify) {
  auto s = driveSettings(); const auto bytes = Fixtures::wav(); LittleFS.put(wav, bytes); newUploadReplies(bytes);
  Host::onConnect = [](size_t index) {
    if (index == 4) { auto saved = load(); CHECK(saved.wavIdReserved && !saved.wavUploaded && std::string(saved.wavDriveId) == "new_wav"); }
    if (index == 6) { auto saved = load(); CHECK(saved.wavSessionUrl == "https://www.googleapis.com/upload/session" && !saved.wavUploaded); }
    if (index == 7) { auto saved = load(); CHECK(saved.wavBytesUploaded == saved.wavSize && !saved.wavUploaded && !saved.fullySynced); }
  };
  GDriveClient client; CHECK(client.uploadNote(s, wav)); CHECK(Host::requests.size() == 8 && Host::responses.empty());
  CHECK(Host::requests[3].find("GET /drive/v3/files/generateIds?") == 0);
  CHECK(Host::requests[5].find("POST /upload/drive/v3/files?") == 0 && Host::requests[5].find("\"id\":\"new_wav\"") != std::string::npos);
  CHECK(Host::requests[6].find("PUT /upload/session ") == 0 && Host::requests[6].find(bytes) != std::string::npos);
  CHECK(Host::requests[7].find("GET /drive/v3/files/new_wav?") == 0);
  auto saved = load(); CHECK(saved.fullySynced && saved.wavUploaded && !saved.wavIdReserved && saved.wavSessionUrl.isEmpty());
  CHECK(std::string(saved.wavMd5) == Host::mockFileHash(bytes).c_str() && !GDriveClient::needsUpload(wav, false));
}
TEST(drive_ack_without_matching_get_hash_is_not_success) {
  auto s = driveSettings(); LittleFS.put(wav, Fixtures::wav()); newUploadReplies(Fixtures::wav(), true);
  GDriveClient client; CHECK(!client.uploadNote(s, wav)); auto saved = load();
  CHECK(!saved.fullySynced && !saved.wavUploaded && std::string(saved.wavDriveId) == "new_wav");
  CHECK(std::string(saved.wavMd5) == Host::mockFileHash(Fixtures::wav()).c_str());
  CHECK(saved.wavSessionUrl.isEmpty() && saved.wavBytesUploaded == 0 && GDriveClient::needsUpload(wav, false));
}
TEST(drive_auth_generation_change_regenerates_id_and_discards_old_session) {
  uploaded(); Host::rebootNvs(); auto s = driveSettings();
  auto old = load(); old.wavUploaded = old.fullySynced = false; old.wavIdReserved = true;
  old.wavSessionUrl = "https://www.googleapis.com/old_session"; old.wavBytesUploaded = 13; CHECK(old.save(wav));
  CHECK(GDriveClient::needsUpload(wav, false)); newUploadReplies(Fixtures::wav());
  GDriveClient client; CHECK(client.uploadNote(s, wav)); auto saved = load();
  CHECK(saved.authGeneration == s.get().driveAuthGeneration && saved.fullySynced && std::string(saved.wavDriveId) == "new_wav");
  for (const auto &request : Host::requests) CHECK(request.find("old_wav") == std::string::npos && request.find("old_session") == std::string::npos);
}
TEST(drive_source_change_updates_snapshot_retains_id_and_uses_patch) {
  uploaded(); Host::rebootNvs(); seedString("driveRefTok", "refresh"); seedString("driveFolder", "folder");
  auto s = settings(); const auto old = Fixtures::wav(); auto changed = old; changed.back() = 1; LittleFS.put(wav, changed);
  oauth(); folder(); reply(remote("old_wav", old, Host::mockFileHash(old))); reply(remote("old_wav", old, Host::mockFileHash(old)));
  reply("{}", 200, "Location: https://www.googleapis.com/upload/patch\r\n"); reply("{}", 200);
  reply(remote("old_wav", changed, Host::mockFileHash(changed)));
  GDriveClient client;
  if (!client.uploadNote(s, wav)) throw std::runtime_error(std::string(client.lastError().c_str()) +
      "; requests=" + std::to_string(Host::requests.size()) +
      (Host::requests.empty() ? "" : "; last=" + Host::requests.back()));
  CHECK(Host::requests.size() == 7);
  CHECK(Host::requests[4].find("PATCH /upload/drive/v3/files/old_wav?") == 0);
  CHECK(std::string(load().wavMd5) == Host::mockFileHash(changed).c_str() && load().fullySynced);
}
TEST(drive_persistence_failure_blocks_network_and_preserves_old_state) {
  auto s = driveSettings(); LittleFS.put(wav, Fixtures::wav()); NoteSyncState state; CHECK(state.save(wav));
  const auto old = LittleFS.bytes(sync); LittleFS.faults->failRename = tmp; oauth();
  GDriveClient client; CHECK(!client.uploadNote(s, wav) && Host::requests.empty());
  CHECK(LittleFS.bytes(sync) == old && LittleFS.exists(tmp));
}
TEST(drive_reservation_and_session_commit_failure_block_next_mutation) {
  for (size_t index : {size_t(3), size_t(5)}) {
    Host::resetClock(); Host::resetNetwork(); Host::resetNvs(); LittleFS.reset();
    auto s = driveSettings(); LittleFS.put(wav, Fixtures::wav()); newUploadReplies(Fixtures::wav());
    std::string committed;
    Host::onConnect = [index, &committed](size_t current) {
      if (current == index) { committed = LittleFS.bytes(sync); LittleFS.faults->failRename = tmp; }
    };
    GDriveClient client; CHECK(!client.uploadNote(s, wav));
    CHECK(Host::requests.size() == index + 1 && LittleFS.bytes(sync) == committed);
    LittleFS.faults->failRename.clear(); auto recovered = load();
    CHECK(recovered.wavIdReserved && !recovered.wavUploaded && !recovered.fullySynced);
    CHECK(std::string(recovered.wavDriveId) == "new_wav");
    CHECK(recovered.wavSessionUrl.isEmpty() == (index == 3));
  }
}
TEST(drive_pairing_strict_token_capacity_and_nvs_failure) {
  for (size_t length : {size_t(512), size_t(513)}) {
    Host::resetNvs(); Host::resetNetwork(); auto s = settings(); CHECK(s.saveDriveApp("client", "secret"));
    reply(R"({"device_code":"device","user_code":"CODE","verification_uri":"https://google.test","expires_in":120,"interval":1})");
    reply(std::string("{\"refresh_token\":\"") + std::string(length, 't') + "\",\"access_token\":\"access\",\"token_type\":\"Bearer\"}");
    GDriveClient client; CHECK(client.pairDevice(s, nullptr) == (length == 512));
    CHECK(s.hasDriveAuth() == (length == 512) && Host::requests.size() == 2);
  }
  Host::resetNvs(); Host::resetNetwork(); auto s = settings(); CHECK(s.saveDriveApp("client", "secret"));
  Host::failPuts.insert("cfg/driveRefTok");
  reply(R"({"device_code":"device","user_code":"CODE","verification_url":"https://google.test","expires_in":120,"interval":1})");
  reply(R"({"refresh_token":"token","access_token":"access","token_type":"Bearer"})");
  GDriveClient client; CHECK(!client.pairDevice(s, nullptr) && !s.hasDriveAuth() && client.lastError().length());
}
} // namespace

int main() {
  size_t failed = 0;
  for (const auto &test : tests()) {
    Host::resetClock(); Host::resetNetwork(); Host::resetNvs(); LittleFS.reset();
    try { test.run(); }
    catch (const std::exception &e) { ++failed; std::cout << "FAIL " << test.name << ": " << e.what() << '\n'; }
  }
  std::cout << "Storage/Drive host tests: " << tests().size() - failed << '/' << tests().size()
            << " OK" << (failed ? " (FALHOU)" : "") << '\n';
  return failed ? 1 : 0;
}
