// The micro SD card, if there is one: music, soundscapes, meditations and photo albums the speaker plays itself.
// The speaker works the same without a card; then present() is false and the media features stay hidden.
//
// Layout (docs/MEDIA.md in server-docker): /Music/<album>/, /Soundscapes/, /Meditations/, /Photos/<album>/; a folder's
// cover.jpg is its artwork. The slot has no card-detect line, so the app polls (poll()): an empty slot is tried every
// few seconds, a mounted card is checked with a raw read of its first sector.
// The card must be FAT32 (FAT16 works too): cards over 32 GB come as exFAT, which the speaker can't read; it says so
// (unreadable()) and format() makes it FAT32.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace card {

// Mounts the card if there is one. Quick when the slot is empty.
void begin();
// Looks again (a card put in or taken out since); remounts and re-reads the catalogue.
void rescan();
// Re-reads what's on it without remounting (after a sync: music may be playing from it).
void refresh();
bool present();
// What poll() found.
enum class Change : uint8_t { None, Inserted, Removed, Unreadable };
// One look at the slot: mounts a card that's appeared; says a mounted one has gone (then stop using it and call
// unmount()). Quick with a card in; with the slot empty it's a mount attempt, so the app spaces these out.
Change poll();
void unmount();
uint32_t freeMb();
// A card is in the slot but its file system can't be read (exFAT, or damaged).
bool unreadable();
// Erases the card and makes it FAT32, with empty Music, Soundscapes, Meditations and Photos folders; then mounts it.
// Works on unreadable cards too. Takes a few seconds. False if there's no card or it failed.
bool format();
// For the status: {"total_mb", "free_mb"}; nothing when there's no card.
void addStatus(JsonObject s);
// What's on it, for Harkenn: {"present", "total_mb", "free_mb", "music": [{"name", "tracks"}], "soundscapes":
// ["Rain", ...], "meditations": [...], "photos": [{"name", "pictures"}]}; {"present": false, "unreadable": true} for
// a card that can't be read.
void catalogue(JsonObject out);
// Goes up whenever the catalogue changes, so the app knows to tell Harkenn.
uint32_t version();

}  // namespace card
