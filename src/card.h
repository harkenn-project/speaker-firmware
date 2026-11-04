// The micro SD card, if there is one: music, soundscapes, meditations and photo albums the speaker plays itself.
// The speaker works the same without a card; then present() is false and the media features stay hidden.
//
// Layout (docs/MEDIA.md in server-docker): /Music/<album>/, /Soundscapes/, /Meditations/, /Photos/<album>/; a folder's
// cover.jpg is its artwork. The slot has no card-detect line, so the card is looked for at boot and on rescan().
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace card {

// Mounts the card if there is one. Quick when the slot is empty.
void begin();
// Looks again (a card put in or taken out since); remounts and re-reads the catalogue.
void rescan();
bool present();
// For the status: {"total_mb", "free_mb"}; nothing when there's no card.
void addStatus(JsonObject s);
// What's on it, for Harkenn: {"present", "total_mb", "free_mb", "music": [{"name", "tracks"}], "soundscapes":
// ["Rain", ...], "meditations": [...], "photos": [{"name", "pictures"}]}.
void catalogue(JsonObject out);
// Goes up whenever the catalogue changes, so the app knows to tell Harkenn.
uint32_t version();

}  // namespace card
