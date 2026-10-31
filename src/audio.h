// Audio: ES8311 codec on I2S (16 kHz, 16-bit mono both ways), speaker amp on GPIO46.
// Playback and capture each run in their own task; every function here is safe to call from any task.
#pragma once
#include <Arduino.h>

namespace audio {

constexpr int SAMPLE_RATE = 16000;

// Built-in sounds, synthesized on the speaker.
enum class Sound : uint8_t { Boot, Activate, Done, Cancel, Error, MuteOn, MuteOff, Tick };

bool begin();  // false if the codec doesn't answer

void setVolume(int percent);  // 0-100; 0 is silent
int volume();

void play(Sound sound);  // queued; volume ticks are dropped while something else is waiting

// Microphone. While muted, nothing is captured and recordings stop.
void setMicMuted(bool muted);
bool micMuted();

// Records into PSRAM until stopRecording(), max_ms, or silence_ms without speech (counted from the start until
// speech is first heard). Returns false if muted or out of memory.
bool startRecording(uint32_t max_ms, uint32_t silence_ms);
void stopRecording();
bool recording();

struct RecordingInfo {
  size_t size = 0;
  uint32_t duration_ms = 0;
  int peak_percent = 0;
  bool speech = false;  // any speech heard at all
  int noise_floor = 0;  // background level, mean absolute amplitude
};
// The last finished recording as a complete WAV file (header included), or nullptr. Owned by the caller,
// free with heap_caps_free().
uint8_t* takeRecordingWav(RecordingInfo* info);

}  // namespace audio
