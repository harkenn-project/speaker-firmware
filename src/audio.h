// Audio: ES8311 codec on I2S, speaker amp on GPIO46. The codec runs at 48 kHz so the speakers can signal each other
// above hearing (wake word arbitration, below); everything else here is 16 kHz, 16-bit mono, filtered to and from it.
// Playback and capture each run in their own task; every function here is safe to call from any task.
#pragma once
#include <Arduino.h>

namespace audio {

constexpr int SAMPLE_RATE = 16000;  // what goes in and out of this module
constexpr int HW_RATE = 48000;      // what the codec runs at

// Sounds. Boot, Activate, Done, Error and Shutdown are the sound pack's slots (boot, wake, done, error, shutdown):
// a clip from Harkenn if one is stored (an empty clip is silence), else synthesized on the speaker like the rest.
enum class Sound : uint8_t { Boot, Activate, Done, Cancel, Error, MuteOn, MuteOff, Tick, Ring, Shutdown,
                             Listen,   // a soft cue when it listens for a reply
                             Claim };  // the inaudible tone (below), at full level whatever the volume

bool begin();  // false if the codec doesn't answer

void setVolume(int percent);  // 0-100; 0 is silent
int volume();

void play(Sound sound);  // queued; volume ticks are dropped while something else is waiting
bool playing();          // a sound is queued or playing (not speech)
uint32_t soundMs(Sound sound);  // how long it plays

// The sound pack's clips (16 kHz 16-bit mono PCM), kept in flash so they play at once, even at boot.
// slot: boot, wake, done, error or shutdown.
bool soundsBegin();  // mount the storage partition
String soundSource(const char* slot);  // the URL the stored clip came from; "" if none
bool storeSound(const char* slot, const String& url, const uint8_t* pcm, size_t bytes);  // bytes 0 = silence
void removeSound(const char* slot);  // back to the built-in sound
uint8_t* readSound(const char* slot, size_t* size);  // a stored file, in PSRAM (free with free()); null if none

// Microphone. While muted, nothing is captured and listening stops.
void setMicMuted(bool muted);
bool micMuted();

// Listening: while on, microphone audio is queued for readMic(). Stops on stopListening(), after max_ms, or after
// silence_ms without speech (counted from the start until speech is first heard). False if muted.
bool startListening(uint32_t max_ms, uint32_t silence_ms);
void stopListening();
bool listening();
size_t readMic(uint8_t* dst, size_t max_bytes);  // 16-bit PCM queued since the last call; never blocks

struct ListenInfo {
  uint32_t duration_ms = 0;
  int peak_percent = 0;
  bool speech = false;  // any speech heard at all
  int noise_floor = 0;  // background level, mean absolute amplitude
  uint32_t dropped_ms = 0;  // audio lost because it wasn't read fast enough
};
ListenInfo listenInfo();  // about the current or last listening session

// Speech from Harkenn (16 kHz, 16-bit mono): startSpeech(), writeSpeech() as it arrives, endSpeech() when all is
// sent. speaking() stays true until it has been played. stopSpeech() cuts it off.
void startSpeech();
size_t writeSpeech(const uint8_t* pcm, size_t bytes);  // returns bytes accepted (the rest didn't fit)
void endSpeech();
void stopSpeech();
bool speaking();

// Wake word arbitration (app.cpp). When several speakers hear the wake word, the one that heard it loudest plays
// the Claim tone first (CLAIM_HZ, above hearing); the others hear it and stay quiet.
constexpr int CLAIM_HZ = 19000;
constexpr uint32_t CLAIM_MS = 80;
uint32_t claimHeardAt();  // millis() when another speaker's tone was last heard; 0 if never
int claimLevel();         // how strong the strongest tone heard since the last call was (amplitude, out of 32767)
// The loudest 100 ms of the last 1.5 s, in dB below full scale: how loud the wake word was, just after it's heard.
int recentLevelDb();

}  // namespace audio
