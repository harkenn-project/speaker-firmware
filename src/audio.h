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
                             Notify,
                             ToneTest,  // the tone test's sweep (below); queued by startToneTest()
                             Warm };    // switches the amplifier on, silently (warmUp())

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
// A claim is three CLAIM_TONE_MS tones back to back: the signature CLAIM_SIGNATURE_HZ[0] then [1] (down in the band,
// then up: no sweep or stray tone does that), then claimHz(level), whose pitch says how loud the wake word was
// (level 0 the loudest, CLAIM_LEVELS - 1 the quietest). Each tone must last about CLAIM_TONE_MS, with nothing in the
// band just before the first or after the last. The level is in a pitch, not in timing, so a claim can be read as soon
// as it ends: the earlier three spaced-out tones took up to 260 ms to read, and two speakers' claims overlapped.
constexpr int CLAIM_SIGNATURE_HZ[2] = {17400, 19800};
constexpr int CLAIM_BASE_HZ = 18000, CLAIM_STEP_HZ = 300, CLAIM_LEVELS = 6;  // levels at 18.0 to 19.5 kHz
constexpr uint32_t CLAIM_TONE_MS = 15;
constexpr int claimHz(int level) { return CLAIM_BASE_HZ + level * CLAIM_STEP_HZ; }
void claim(int level);  // plays our claim, at full level whatever the volume
uint32_t claimMs();     // how long it takes
// Switch the amplifier on now and keep it on for `ms`, so a claim played in that time comes out at once: switched
// on from cold, the amplifier takes a while to make any sound (a 45 ms claim with 5 ms came out silent).
constexpr uint32_t AMP_WAKE_MS = 100;
void warmUp(uint32_t ms);
struct HeardClaim {
  uint32_t at = 0;    // millis() when it was heard (when it ended); 0 if none yet
  int level = -1;     // 0 = the loudest
  int strength = 0;   // how strong its weakest tone was here (amplitude, out of 32767)
};
HeardClaim lastClaim();  // the last claim heard from another speaker

// For the logs: what the claim detector heard, so they show whether tones reach a speaker at all.
struct ToneEvent {
  enum Kind : uint8_t {
    Claim,     // a whole claim (level)
    Fragment,  // a tone in the band that didn't make up a claim (hz, ms)
    Own,       // our own claim, as our microphone heard it (ms = how long after we played it; -1 if it never did)
  } kind = Claim;
  int hz = 0;
  int level = 0;
  int ms = 0;
  int amplitude = 0;  // peak (out of 32767); for a claim, of its weakest tone
  int noise = 0;      // the reference bands (16.8 and 20.4 kHz) at the time
};
bool takeToneEvent(ToneEvent& e);  // oldest first; false when there are none
int droppedToneEvents();           // how many didn't fit in the queue, since the last call
// The loudest the claim band got since the last call, outside our own claims, even below the detection threshold:
// whether anything at all came through. Two readers (0 and 1) each get their own "since the last call".
struct ClaimBand {
  int amplitude = 0, hz = 0, noise = 0;
};
ClaimBand claimBandPeak(int reader);

// Tone test (Harkenn's "Test tones between speakers"): one speaker plays a 150 ms tone at each of TEST_HZ, 250 ms
// apart after 300 ms of quiet, then a claim at level 0; the others only listen. Every speaker measures each pitch in
// its own window of that schedule (so a harmonic of one tone doesn't count for another), against the same pitch in
// the quiet before. The schedule runs from when the command arrived, so speakers needn't be exactly in step.
constexpr int TEST_TONES = 8;
constexpr int TEST_HZ[TEST_TONES] = {8000, 12000, 15000, 16000, 17000, 18000, 19000, 20000};
constexpr uint32_t TEST_LEAD_MS = 300, TEST_TONE_MS = 150, TEST_STEP_MS = 250;
constexpr uint32_t TEST_CLAIM_AT_MS = TEST_LEAD_MS + TEST_TONES * TEST_STEP_MS + 100;
constexpr uint32_t TEST_MS = TEST_CLAIM_AT_MS + 700;  // the claim, and time for it to arrive and be read
struct ToneTest {
  bool played = false;           // we played the sweep (false: we listened)
  int heard[TEST_TONES] = {};    // the loudest 5 ms block of each pitch in its window (out of 32767)
  int quiet[TEST_TONES] = {};    // the loudest block of each pitch in the quiet before
  int back_ms[TEST_TONES] = {};  // from the start of its slot to the first block well above quiet; -1 if none
};
// false if it can't: the codec runs at 16 kHz (no high tones), the mic is muted, or a test is running.
bool startToneTest(bool play);
bool toneTestResult(ToneTest& r);  // true once, when the test has finished

// The loudest 16 ms of the last 1.5 s, in dB below full scale: how loud the wake word was, just after it's heard.
// `ago_ms`, if given, gets how long ago that was.
int recentLevelDb(uint32_t* ago_ms = nullptr);
// The microphone since the last call: average and loudest level (dBFS, per 16 ms) and the background noise floor
// (mean absolute amplitude), for the logs.
struct MicStats {
  int average_db = 0, loudest_db = 0, noise_floor = 0;
};
MicStats micStats();

}  // namespace audio
