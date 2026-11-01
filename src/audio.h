// Audio: ES8311 codec on I2S (16 kHz, 16-bit mono both ways), speaker amp on GPIO46.
// Playback and capture each run in their own task; every function here is safe to call from any task.
#pragma once
#include <Arduino.h>

namespace audio {

constexpr int SAMPLE_RATE = 16000;

// Sounds. Boot, Activate, Done, Error and Shutdown are the sound pack's slots (boot, wake, done, error, shutdown):
// a clip from Harkenn if one is stored (an empty clip is silence), else synthesized on the speaker like the rest.
enum class Sound : uint8_t { Boot, Activate, Done, Cancel, Error, MuteOn, MuteOff, Tick, Ring, Shutdown,
                             Listen };  // Listen: a soft cue when it listens for a reply

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

}  // namespace audio
