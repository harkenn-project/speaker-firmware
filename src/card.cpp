#include "card.h"

#include <FS.h>
#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

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

constexpr const char* MOUNT = "/sdcard";
bool mounted = false;
bool unreadable_card = false;
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

// The ESP-IDF way in, for what the Arduino SD_MMC library can't do: tell an unreadable card from no card, and
// format. Mounts at the same place; `format` makes a new FAT32 file system if the card has none it can read.
esp_err_t idf_mount(bool format, sdmmc_card_t** card) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = static_cast<gpio_num_t>(pins::SD_CLK);
  slot.cmd = static_cast<gpio_num_t>(pins::SD_CMD);
  slot.d0 = static_cast<gpio_num_t>(pins::SD_D0);
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t config = {};
  config.format_if_mount_failed = format;
  config.max_files = 2;
  config.allocation_unit_size = 32 * 1024;  // big clusters: music files are big, and the FAT stays small
  return esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &config, card);
}

// After the Arduino library couldn't mount: ESP_OK if the card is readable after all (slow on the first try),
// ESP_FAIL if a card answers but its file system can't be read, anything else if there's no card.
esp_err_t probe() {
  sdmmc_card_t* card = nullptr;
  const esp_err_t err = idf_mount(false, &card);
  if (err == ESP_OK) esp_vfs_fat_sdcard_unmount(MOUNT, card);
  return err;
}

bool mount() {
  // 1-bit SD mode on the BOX's own pins (not shared with the screen); D3 is pulled up on the board.
  SD_MMC.setPins(pins::SD_CLK, pins::SD_CMD, pins::SD_D0);
  if (!SD_MMC.begin(MOUNT, true, false, SDMMC_FREQ_DEFAULT, 4)) {
    const esp_err_t err = probe();
    unreadable_card = err == ESP_FAIL;
    if (err != ESP_OK || !SD_MMC.begin(MOUNT, true, false, SDMMC_FREQ_DEFAULT, 4)) return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    return false;
  }
  return true;
}

}  // namespace

void begin() {
  unreadable_card = false;
  mounted = mount();
  if (mounted) {
    LOGI(TAG, "SD card: %llu MB, %llu MB free", SD_MMC.totalBytes() >> 20,
         (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20);
    read_catalogue();
  } else if (unreadable_card) {
    LOGW(TAG, "there's an SD card, but it can't be read (exFAT, or damaged): format it from Harkenn");
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

bool unreadable() { return unreadable_card; }

bool format() {
  if (mounted) SD_MMC.end();
  mounted = false;
  forget();
  LOGW(TAG, "formatting the SD card");
  sdmmc_card_t* card = nullptr;
  esp_err_t err = idf_mount(true, &card);  // an unreadable card is formatted here
  if (err == ESP_OK) {
    err = esp_vfs_fat_sdcard_format(MOUNT, card);  // a readable one here
    if (err == ESP_OK) {
      for (const char* dir : {"/Music", "/Soundscapes", "/Meditations", "/Photos"}) mkdir((String(MOUNT) + dir).c_str(), 0775);
    }
    esp_vfs_fat_sdcard_unmount(MOUNT, card);
  }
  if (err == ESP_OK) LOGI(TAG, "SD card formatted (FAT32)");
  else LOGE(TAG, "couldn't format the SD card: %s", esp_err_to_name(err));
  begin();
  return err == ESP_OK && mounted;
}

void addStatus(JsonObject s) {
  if (!mounted) return;
  JsonObject c = s["card"].to<JsonObject>();
  c["total_mb"] = SD_MMC.totalBytes() >> 20;
  c["free_mb"] = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) >> 20;
}

void catalogue(JsonObject out) {
  out["present"] = mounted;
  if (!mounted) {
    if (unreadable_card) out["unreadable"] = true;
    return;
  }
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
