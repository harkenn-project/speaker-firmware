// The encrypted channel to Harkenn through the relay (docs/RELAY.md in server-docker): the handshake, and sealing and
// opening frames. The connection itself is app.cpp's WebSocket. Only the speaker and Harkenn have the key; the relay
// just passes frames along.
#pragma once
#include <Arduino.h>

namespace relay {

constexpr uint8_t HELLO = 0x01, ANSWER = 0x02, OK = 0x03, DATA = 0x10;
constexpr size_t OVERHEAD = 1 + 8 + 1 + 16;  // type, counter, kind, GCM tag

// Starts a session with this key (64 hex digits). False if the key isn't one.
bool begin(const String& key_hex);
// Harkenn's HELLO (0x01 + 16-byte nonce) in, our ANSWER out (33 bytes). False if it isn't a HELLO.
bool answer(const uint8_t* hello, size_t len, uint8_t out[33]);
// Harkenn's OK: true if it proves Harkenn has the key; the session keys are ready then.
bool confirm(const uint8_t* ok, size_t len);
bool ready();
void end();

// A DATA frame of `kind` ('T' text, 'B' binary) with this payload, into `out` (len + OVERHEAD bytes). Returns its
// length, 0 if not ready.
size_t seal(char kind, const uint8_t* data, size_t len, uint8_t* out);
// Opens a DATA frame in place: its kind and payload (pointing into `frame`). False if it's forged, replayed or out of
// order: the session is over then.
bool open(uint8_t* frame, size_t len, char* kind, uint8_t** data, size_t* data_len);

}  // namespace relay
