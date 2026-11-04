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
                             Claim,    // the inaudible tone (below), at full level whatever the volume
                             TimerDone, Alarm,  // at least at a level you'd hear, whatever the volume
                             Notify };

// false if the codec doesn't answer. `ultrasonic`: run the codec at 48 kHz, for wake word arbitration (below);
// otherwise at 16 kHz, and there are no claims.
bool begin(bool ultrasonic);
bool ultrasonic();

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

// Music from the SD card (player.cpp): mono 16-bit PCM at hwRate(), the codec's rate, so it isn't squeezed into
// speech's 16 kHz. Sounds and speech play over it (it waits meanwhile). It fades out while any hold is on and fades
// back in when they're all off; what's buffered stays, so a pause resumes where it left off.
constexpr uint8_t HOLD_PAUSED = 1;  // the player is paused
constexpr uint8_t HOLD_VOICE = 2;   // listening, talking or ringing (app.cpp)
int hwRate();
size_t musicSpace();  // samples writeMusic() would take now
size_t musicQueued();  // samples waiting to be played
size_t writeMusic(const int16_t* pcm, size_t samples);  // returns samples accepted
void holdMusic(uint8_t reason, bool on);
bool musicHeld();
void clearMusic();  // drops what's buffered (a new track, or stop)
bool musicPlaying();  // music is coming out of the speaker now

// Wake word arbitration (app.cpp): speakers that heard the wake word tell each other how loudly, above hearing.
// A claim is three short tones: CLAIM_HZ[0]; CLAIM_HZ[1] exactly CLAIM_AB_MS later (the signature: a stray tone
// can't pass for it); then CLAIM_HZ[2], CLAIM_BC_MS + level x CLAIM_SLOT_MS after that, which says how loud the
// wake word was (level 0 the loudest, CLAIM_LEVELS - 1 the quietest).
constexpr int CLAIM_HZ[3] = {18600, 19200, 19800};
constexpr uint32_t CLAIM_TONE_MS = 30, CLAIM_AB_MS = 50, CLAIM_BC_MS = 40, CLAIM_SLOT_MS = 10;
constexpr int CLAIM_LEVELS = 16;
void claim(int level);        // plays our claim, at full level whatever the volume
uint32_t claimMs(int level);  // how long it takes
struct HeardClaim {
  uint32_t at = 0;    // millis() when it was heard (its last tone); 0 if none yet
  int level = -1;     // 0 = the loudest
  int strength = 0;   // how strong its first tone was here (amplitude, out of 32767)
};
HeardClaim lastClaim();  // the last claim heard from another speaker
// The loudest 16 ms of the last 1.5 s, in dB below full scale: how loud the wake word was, just after it's heard.
int recentLevelDb();
// The microphone since the last call: average and loudest level (dBFS, per 16 ms) and the background noise floor
// (mean absolute amplitude), for the logs.
struct MicStats {
  int average_db = 0, loudest_db = 0, noise_floor = 0;
};
MicStats micStats();

}  // namespace audio
