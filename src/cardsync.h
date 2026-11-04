// Keeping the SD card in step with Harkenn's media library (cardsync.py in server-docker; PROTOCOL.md §12).
//
// Harkenn sends a plan: every file it wants on the card, with sizes and checksums. The card keeps
// /.harkenn/manifest.json, the list of files Harkenn put there. A sync deletes Harkenn's files that aren't in the plan,
// fetches what's missing or changed a piece at a time over the WebSocket, and leaves everything else alone: files
// someone copied onto the card themselves are never touched.
//
// It fetches only while the speaker is idle (not listening, talking or ringing), resumes a half-fetched file, and
// checks each file's SHA-256 before it counts. Runs in the main task: loop() from the main loop, the handlers from
// the WebSocket callbacks.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace cardsync {

using Sender = void (*)(JsonDocument& doc);

void begin(Sender send);
// A plan arrived ({"plan", "files": [{path, size, sha256}]}): it replaces any sync under way.
void onPlan(const char* json, size_t len);
// The header of a piece Harkenn sends ({"path", "offset", "bytes", "error"}); the bytes follow as binary frames.
void onChunk(JsonDocument& doc);
// A binary frame: true if it was a piece of a file (then it's written to the card).
bool onBinary(const uint8_t* data, size_t len);
// Does the next step if there's one; `idle`: the speaker isn't listening, talking or ringing.
void loop(bool idle, bool connected);
// For the card report: "sync": true, "plan" (the last plan finished) and "managed" (Harkenn's files).
void describe(JsonObject card);
bool busy();
// Drops any sync under way (the card is about to be formatted or swapped).
void reset();
// An allocator in PSRAM, for documents that can get big (the card report lists Harkenn's files).
ArduinoJson::Allocator* psramAllocator();

}  // namespace cardsync
