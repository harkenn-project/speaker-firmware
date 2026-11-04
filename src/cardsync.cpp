#include "cardsync.h"

#include <FS.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include <vector>

#include "card.h"
#include "logbuf.h"
#include "player.h"

namespace cardsync {
namespace {

constexpr const char* TAG = "cardsync";
constexpr const char* MANIFEST = "/.harkenn/manifest.json";
constexpr size_t GET_BYTES = 32 * 1024;       // asked for at a time
constexpr int IN_FLIGHT = 2;                  // pieces asked for ahead: the next is on its way while one's written
constexpr uint32_t CHUNK_TIMEOUT_MS = 30000;  // no answer: ask again
constexpr uint32_t REPORT_MS = 1000;

// The plan and the manifest can list hundreds of files: they live in PSRAM, not the scarce internal RAM.
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator psram;

enum class Step : uint8_t { Idle, Start, Fetch, Wait, Done, Error };

Sender sender = nullptr;
JsonDocument plan(&psram);      // {"plan", "files": [{path, size, sha256}]}
JsonDocument manifest(&psram);  // {"plan": the last one finished, "files": {path: sha256}}
bool manifest_loaded = false;
std::vector<uint16_t> todo;     // indexes into the plan's files still to fetch
size_t todo_at = 0;
Step step = Step::Idle;
String error;
uint64_t total_bytes = 0, done_bytes = 0;

// the file being fetched
File part;
uint32_t offset = 0;     // written so far
uint32_t asked_to = 0;   // asked for up to here
int in_flight = 0;       // pieces asked for and not yet all here
mbedtls_sha256_context sha;
bool sha_on = false;
uint32_t expect = 0, got = 0;  // the piece on its way
uint32_t asked_at = 0;
bool paused_reported = false;
uint32_t last_report = 0;

JsonObject want(size_t i) { return plan["files"][i]; }
JsonObject current() { return want(todo[todo_at]); }
String path_of(JsonObject w) { return String("/") + (w["path"] | ""); }
const char* plan_id() { return plan["plan"] | ""; }

void load_manifest() {
  manifest.clear();
  File f = SD_MMC.open(MANIFEST);
  if (f) {
    if (deserializeJson(manifest, f)) manifest.clear();
    f.close();
  }
  if (!manifest["files"].is<JsonObject>()) manifest["files"].to<JsonObject>();
  manifest_loaded = true;
}

void save_manifest() {
  SD_MMC.mkdir("/.harkenn");
  File f = SD_MMC.open("/.harkenn/manifest.tmp", FILE_WRITE);
  if (!f) {
    LOGW(TAG, "couldn't write the manifest");
    return;
  }
  serializeJson(manifest, f);
  f.close();
  SD_MMC.remove(MANIFEST);
  SD_MMC.rename("/.harkenn/manifest.tmp", MANIFEST);
}

// Makes the folders a file goes in.
void make_dirs(const String& path) {
  for (int i = path.indexOf('/', 1); i > 0; i = path.indexOf('/', i + 1)) SD_MMC.mkdir(path.substring(0, i));
}

void report(const char* state, const char* file = "") {
  last_report = millis();
  if (!sender) return;
  JsonDocument doc;
  doc["type"] = "card_sync";
  doc["plan"] = plan_id();
  doc["state"] = state;
  doc["done_bytes"] = done_bytes;
  doc["total_bytes"] = total_bytes;
  if (*file) doc["file"] = file;
  if (error.length()) doc["error"] = error;
  sender(doc);
}

void stop_hash() {
  if (sha_on) mbedtls_sha256_free(&sha);
  sha_on = false;
}

void fail(const String& why) {
  error = why;
  step = Step::Error;
  if (part) part.close();
  stop_hash();
  LOGW(TAG, "sync stopped: %s", why.c_str());
  report("error");
}

String hex(const uint8_t* digest) {
  static const char* digits = "0123456789abcdef";
  String s;
  for (int i = 0; i < 32; i++) s += digits[digest[i] >> 4], s += digits[digest[i] & 15];
  return s;
}

// Starts the next file: opens its .part (a piece fetched before is hashed and carried on from) and begins.
void open_next() {
  if (todo_at >= todo.size()) {
    step = Step::Done;
    return;
  }
  JsonObject w = current();
  const uint32_t size = w["size"] | 0;
  const String target = path_of(w), tmp = target + ".part";
  make_dirs(target);
  stop_hash();
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  sha_on = true;
  offset = 0;
  File existing = SD_MMC.open(tmp);
  if (existing && existing.size() <= size) {
    static uint8_t buf[2048];
    int n;
    while ((n = existing.read(buf, sizeof(buf))) > 0) mbedtls_sha256_update(&sha, buf, n), offset += n;
  }
  if (existing) existing.close();
  if (offset > size) {
    SD_MMC.remove(tmp);
    offset = 0;
  }
  part = SD_MMC.open(tmp, offset ? FILE_APPEND : FILE_WRITE);
  if (!part) {
    fail("couldn't write to the card");
    return;
  }
  done_bytes += offset;
  asked_to = offset;
  in_flight = 0;
  step = Step::Fetch;
}

// A file is all here: check it, put it in place, note it in the manifest.
void finish_file() {
  JsonObject w = current();
  part.close();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  stop_hash();
  const String target = path_of(w), tmp = target + ".part";
  if (hex(digest) != String(w["sha256"] | "")) {
    SD_MMC.remove(tmp);
    fail(String("a file didn't match its checksum: ") + (w["path"] | ""));
    return;
  }
  if (player::playingPath() == target) player::stop();  // never swap a file out from under the player
  SD_MMC.remove(target);
  SD_MMC.rename(tmp, target);
  manifest["files"][w["path"].as<const char*>()] = w["sha256"].as<const char*>();
  save_manifest();
  LOGI(TAG, "got %s (%u KB)", w["path"].as<const char*>(), static_cast<unsigned>((w["size"] | 0) / 1024));
  todo_at++;
  step = Step::Start;
}

bool in_plan(const char* path) {
  for (JsonObject w : plan["files"].as<JsonArray>()) {
    if (strcmp(w["path"] | "", path) == 0) return true;
  }
  return false;
}

// A new plan's first steps: delete Harkenn's unwanted files, work out what to fetch, check there's room.
void start_plan() {
  if (!card::present()) {
    fail("no SD card");
    return;
  }
  load_manifest();  // fresh: the card may have been swapped
  JsonObject files = manifest["files"].as<JsonObject>();
  std::vector<String> drop;
  for (JsonPair kv : files) {
    if (!in_plan(kv.key().c_str())) drop.push_back(kv.key().c_str());
  }
  for (const String& rel : drop) {
    const String p = "/" + rel;
    if (player::playingPath() == p) player::stop();
    SD_MMC.remove(p);
    files.remove(rel);
    const int slash = p.lastIndexOf('/');
    if (slash > 0 && p.startsWith("/Music/")) SD_MMC.rmdir(p.substring(0, slash));  // only goes if it's empty
    LOGI(TAG, "removed %s", rel.c_str());
  }
  if (!drop.empty()) save_manifest();
  todo.clear();
  todo_at = 0;
  total_bytes = done_bytes = 0;
  JsonArray wants = plan["files"].as<JsonArray>();
  for (size_t i = 0; i < wants.size(); i++) {
    JsonObject w = wants[i];
    const uint32_t size = w["size"] | 0;
    File f = SD_MMC.open(path_of(w));
    const bool ok = f && f.size() == size && String(files[w["path"].as<const char*>()] | "") == (w["sha256"] | "");
    if (f) f.close();
    if (!ok) todo.push_back(i), total_bytes += size;
  }
  const uint64_t free_bytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  if (total_bytes > free_bytes + 1024 * 1024) {
    fail("not enough room on the card");
    return;
  }
  LOGI(TAG, "plan %s: %u files, %u to fetch (%u KB), %u removed", plan_id(), wants.size(), todo.size(),
       static_cast<unsigned>(total_bytes / 1024), drop.size());
  step = Step::Start;
}

}  // namespace

void begin(Sender send) { sender = send; }

void onPlan(const char* json, size_t len) {
  if (part) part.close();
  stop_hash();
  plan.clear();
  if (deserializeJson(plan, json, len)) {
    LOGW(TAG, "couldn't read the plan");
    plan.clear();
    return;
  }
  // only plain paths inside the card, with a checksum
  JsonArray files = plan["files"].as<JsonArray>();
  for (size_t i = files.size(); i-- > 0;) {
    const String p = files[i]["path"] | "";
    if (!p.length() || p.startsWith("/") || p.indexOf("..") >= 0 || strlen(files[i]["sha256"] | "") != 64) {
      files.remove(i);
    }
  }
  error = "";
  expect = got = 0;
  start_plan();
}

void onChunk(JsonDocument& doc) {
  if ((step != Step::Wait && step != Step::Fetch) || todo_at >= todo.size() || expect) return;
  JsonObject w = current();
  if (strcmp(w["path"] | "", doc["path"] | "") || (doc["offset"] | -1) != static_cast<long>(offset)) return;
  expect = doc["bytes"] | 0;
  got = 0;
  if (!expect) fail(String("Harkenn couldn't send ") + (w["path"] | "") + ": " + (doc["error"] | "no data"));
}

bool onBinary(const uint8_t* data, size_t len) {
  if ((step != Step::Wait && step != Step::Fetch) || !expect || got >= expect) return false;
  const size_t n = min<size_t>(len, expect - got);
  if (part.write(data, n) != n) {
    fail("couldn't write to the card (is it full?)");
    return true;
  }
  mbedtls_sha256_update(&sha, data, n);
  got += n;
  if (got == expect) {
    offset += got;
    done_bytes += got;
    expect = got = 0;
    in_flight = max(0, in_flight - 1);
    asked_at = millis();
    if (offset >= (current()["size"] | 0)) finish_file();
    else step = Step::Fetch;
  }
  return true;
}

void loop(bool idle, bool connected) {
  switch (step) {
    case Step::Idle:
    case Step::Error:
      return;
    case Step::Start:
      open_next();
      return;
    case Step::Wait:
    case Step::Fetch: {
      if (in_flight && static_cast<int32_t>(millis() - asked_at) > static_cast<int32_t>(CHUNK_TIMEOUT_MS)) {
        // nothing for a while (the connection dropped?): start the file again from what's really in its .part
        part.close();
        done_bytes -= offset;
        expect = got = 0;
        in_flight = 0;
        step = Step::Start;
        return;
      }
      if (!connected) return;
      JsonObject w = current();
      const uint32_t size = w["size"] | 0;
      if (!idle) {  // what's asked for still arrives; nothing more until it's idle again
        if (!paused_reported) report("paused", w["path"] | ""), paused_reported = true;
        return;
      }
      paused_reported = false;
      while (in_flight < IN_FLIGHT && asked_to < size) {
        JsonDocument doc;
        doc["type"] = "card_get";
        doc["path"] = w["path"];
        doc["offset"] = asked_to;
        doc["length"] = min<uint32_t>(GET_BYTES, size - asked_to);
        if (sender) sender(doc);
        asked_to += doc["length"].as<uint32_t>();
        if (!in_flight) asked_at = millis();
        in_flight++;
      }
      step = Step::Wait;
      if (millis() - last_report > REPORT_MS) report("syncing", w["path"] | "");
      return;
    }
    case Step::Done:
      manifest["plan"] = plan_id();
      save_manifest();
      LOGI(TAG, "card in step with plan %s", plan_id());
      step = Step::Idle;
      report("done");
      card::refresh();  // tells Harkenn what's on it now, with this plan
      return;
  }
}

void describe(JsonObject out) {
  if (!card::present()) return;
  if (!busy()) load_manifest();  // fresh, unless a sync is using it: the card may have been swapped
  out["sync"] = true;
  out["plan"] = manifest["plan"] | "";
  JsonArray m = out["managed"].to<JsonArray>();
  size_t n = 0;
  for (JsonPair kv : manifest["files"].as<JsonObject>()) {
    if (++n > 2000) break;
    m.add(kv.key().c_str());
  }
}

bool busy() { return step == Step::Fetch || step == Step::Wait || step == Step::Start; }

ArduinoJson::Allocator* psramAllocator() { return &psram; }

void reset() {
  if (part) part.close();
  stop_hash();
  plan.clear();
  todo.clear();
  expect = got = 0;
  manifest_loaded = false;
  step = Step::Idle;
}

}  // namespace cardsync
