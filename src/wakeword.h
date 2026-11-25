// Wake word, on the speaker: a streaming microWakeWord-style model (TensorFlow Lite Micro) listens to the microphone
// while the speaker is idle, so no audio leaves it until the wake word is heard.
//
// The audio task feeds 16 kHz samples with feed(); a task of its own turns them into features (TFLM's micro audio
// frontend: 40 channels from 30 ms windows every 10 ms) and runs the model on them. Every function is safe to call
// from any task.
#pragma once
#include <Arduino.h>

namespace wakeword {

bool begin();  // start the task (nothing is detected until a model is loaded)

// Load a model (a .tflite file; copied, so the caller's buffer can go). A detection needs the average of the last
// `window` probabilities to reach `cutoff` (0-1). `arena` is the tensor memory the model needs, in bytes.
bool load(const uint8_t* model, size_t size, float cutoff, int window, size_t arena);
void unload();
bool loaded();

// "Stop": a second model on the same features, listening only while something rings (timers, alarms).
bool loadStop(const uint8_t* model, size_t size, float cutoff, int window, size_t arena);
void unloadStop();
bool stopLoaded();
void setStopListening(bool on);
bool takeStop();  // true once per "stop" heard

void feed(const int16_t* samples, size_t count);  // from the audio task; never blocks, drops samples when behind
void setListening(bool on);  // pause (false) while the speaker is busy, so it doesn't wake itself up
bool takeDetection();        // true once per detection

// Wake clips: while on, the last few seconds of audio the wake word heard are kept (in PSRAM), and the last
// CLIP_SAMPLES are copied at every detection, so Harkenn can be sent what set it off (false wakes become training
// data).
constexpr size_t CLIP_SAMPLES = 3 * 16000;
void setClips(bool on);

// Wake word test: listens for TEST_MS from now (call it once any cue has finished playing) without counting as a
// detection, and keeps what it heard (up to TEST_SAMPLES) with the highest averaged probability, for takeClip().
// Stops early just after the wake word is heard. Works with clips off too (the buffers are kept only meanwhile).
constexpr size_t TEST_SAMPLES = 4 * 16000;
constexpr uint32_t TEST_MS = 4000;
bool startTest();   // false if there's no model or no memory
bool testing();     // started and not finished yet
void cancelTest();  // gives up (no clip)

struct Clip {
  size_t samples = 0;     // 0: none
  float probability = 0;  // a detection's averaged probability, or a test's highest (0-1)
  bool test = false;      // from startTest()
  bool detected = false;  // a test: the wake word would have woken the speaker
};
// The newest clip not taken yet, into `out` (room for TEST_SAMPLES), oldest sample first.
Clip takeClip(int16_t* out);

struct Stats {
  uint32_t inferences = 0;
  uint32_t avg_infer_us = 0;
  float max_probability = 0;  // since the last call to stats()
};
Stats stats();

}  // namespace wakeword
