// The media player: music albums, soundscapes and meditations from the micro SD card (card.h).
//
// - Music plays an album's tracks in order (or shuffled); at the end it stops, or starts over (repeat all), or one
//   track plays round and round (repeat one).
// - A soundscape loops until it's stopped, changed, or the sleep timer runs out. WAV loops with no gap at all.
// - A meditation plays once and fades out.
// Any of them can have a sleep timer: it fades out over a few seconds and stops.
//
// The player task decodes MP3 (minimp3) or WAV into the audio module's music stream (audio.h), resampled to the
// codec's rate. Music fades out while the speaker listens or talks, and fades back in after. Commands come from
// Harkenn (docs/PROTOCOL.md, "The media player") and are safe to call from any task.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace player {

enum class Kind : uint8_t { Music, Soundscape, Meditation };
enum class Repeat : uint8_t { Off, All, One };

void begin();

// Starts `album` (a music album folder, or a soundscape's or meditation's name) at track `track` (0 = the first).
// The repeat mode defaults by kind: all for music, one for soundscapes, off for meditations.
void play(Kind kind, const String& album, int track = 0, bool shuffle = false);
void pause();
void resume();
void stop();
void next();
void previous();
void setRepeat(Repeat repeat);
void sleepAfter(uint32_t minutes);  // fade out and stop then; 0 turns it off

bool active();  // playing or paused
// The file playing or paused ("/Music/Background/01 X.mp3"), or "": so a card sync never pulls it away.
String playingPath();
// Goes up on every change worth telling Harkenn about.
uint32_t version();
// {state: "playing"|"paused"|"stopped", kind, album, title, track, tracks, repeat, shuffle, sleep_s, error}
void toJson(JsonObject out);

const char* kindName(Kind kind);
bool parseKind(const String& name, Kind* out);
bool parseRepeat(const String& name, Repeat* out);

}  // namespace player
