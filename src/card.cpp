#include "card.h"

#include <FS.h>
#include <SD_MMC.h>

#include <vector>

#include "board.h"
#include "logbuf.h"

namespace card {
namespace {

constexpr const char* TAG = "card";
constexpr size_t MAX_ENTRIES = 64;  // albums or files listed per folder

struct Album {
  String name;
  uint16_t count = 0;
};

bool mounted = false;
std::vector<Album> music, photos;
std::vector<String> soundscapes, meditations;
volatile uint32_t changes = 0;

bool has_extension(const String& name, std::initializer_list<const char*> extensions) {
  String lower = name;
  lower.toLowerCase();
  for (const char* e : extensions) {
    if (lower.endsWith(e)) return true;
  }
  return false;
}

bool is_audio(const String& name) { return has_extension(name, {".mp3", ".wav"}); }
bool is_picture(const String& name) { return has_extension(name, {".jpg", ".jpeg"}); }

// Without the extension, for saying and showing: "Rain.mp3" -> "Rain".
String title(const String& name) {
  const int dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(0, dot) : name;
}

// Files in a folder that pass `keep`, skipping hidden ones (e.g. ._Rain.mp3 from a Mac).
uint16_t count_files(const String& path, bool (*keep)(const String&)) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) return 0;
  uint16_t n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String name = f.name();
    if (!f.isDirectory() && !name.startsWith(".") && keep(name)) n++;
  }
  return n;
}

void list_albums(const char* path, bool (*keep)(const String&), std::vector<Album>& out) {
  out.clear();
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f && out.size() < MAX_ENTRIES; f = dir.openNextFile()) {
    const String name = f.name();
    if (!f.isDirectory() || name.startsWith(".")) continue;
    Album a;
    a.name = name;
    a.count = count_files(String(path) + "/" + name, keep);
    if (a.count) out.push_back(a);
  }
}

void list_files(const char* path, std::vector<String>& out) {
  out.clear();
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f && out.size() < MAX_ENTRIES; f = dir.openNextFile()) {
    const String name = f.name();
    if (!f.isDirectory() && !name.startsWith(".") && is_audio(name)) out.push_back(title(name));
  }
}

void read_catalogue() {
  list_albums("/Music", is_audio, music);
  list_albums("/Photos", is_picture, photos);
  list_files("/Soundscapes", soundscapes);
  list_files("/Meditations", meditations);
  LOGI(TAG, "%u music albums, %u soundscapes, %u meditations, %u photo albums", music.size(), soundscapes.size(),
       meditations.size(), photos.size());
}

void forget() {
  music.clear();
  photos.clear();
  soundscapes.clear();
  meditations.clear();
}

bool mount() {
  // 1-bit SD mode on the BOX's own pins (not shared with the screen); D3 is pulled up on the board.
  SD_MMC.setPins(pins::SD_CLK, pins::SD_CMD, pins::SD_D0);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 4)) return false;
  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    return false;
  }
  return true;
}

}  // namespace

void begin() {
  mounted = mount();
  if (mounted) {
    LOGI(TAG, "SD card: %llu MB, %llu MB free", SD_MMC.totalBytes() >> 20,
         (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20);
    read_catalogue();
  } else {
    LOGI(TAG, "no SD card");
  }
  changes++;
}

void rescan() {
  if (mounted) SD_MMC.end();
  forget();
  begin();
}

bool present() { return mounted; }

void addStatus(JsonObject s) {
  if (!mounted) return;
  JsonObject c = s["card"].to<JsonObject>();
  c["total_mb"] = SD_MMC.totalBytes() >> 20;
  c["free_mb"] = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;
}

void catalogue(JsonObject out) {
  out["present"] = mounted;
  if (!mounted) return;
  out["total_mb"] = SD_MMC.totalBytes() >> 20;
  out["free_mb"] = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;
  JsonArray m = out["music"].to<JsonArray>();
  for (const auto& a : music) {
    JsonObject o = m.add<JsonObject>();
    o["name"] = a.name;
    o["tracks"] = a.count;
  }
  JsonArray s = out["soundscapes"].to<JsonArray>();
  for (const auto& n : soundscapes) s.add(n);
  JsonArray d = out["meditations"].to<JsonArray>();
  for (const auto& n : meditations) d.add(n);
  JsonArray p = out["photos"].to<JsonArray>();
  for (const auto& a : photos) {
    JsonObject o = p.add<JsonObject>();
    o["name"] = a.name;
    o["pictures"] = a.count;
  }
}

uint32_t version() { return changes; }

}  // namespace card
