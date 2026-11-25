#include "wakeword.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <freertos/stream_buffer.h>

#include <utility>

#include "logbuf.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace wakeword {
namespace {

constexpr const char* TAG = "wake";
constexpr int SAMPLE_RATE = 16000;
constexpr int FEATURES = 40;                      // per 10 ms step
constexpr size_t SAMPLE_BUFFER_BYTES = 32 * 1024;  // 1 s of slack
constexpr uint32_t REFRACTORY_MS = 2000;            // after a detection, don't detect again for a while
constexpr uint32_t WARM_UP_MS = 1000;               // after a pause, until the model has heard enough new audio
constexpr int RESOURCE_VARIABLES = 20;

// The features the models were trained on: TFLM's micro frontend with these settings.
void configure(FrontendConfig& c) {
  c.window.size_ms = 30;
  c.window.step_size_ms = 10;
  c.filterbank.num_channels = FEATURES;
  c.filterbank.lower_band_limit = 125.0f;
  c.filterbank.upper_band_limit = 7500.0f;
  c.filterbank.output_scale_shift = 0;
  c.noise_reduction.smoothing_bits = 10;
  c.noise_reduction.even_smoothing = 0.025f;
  c.noise_reduction.odd_smoothing = 0.06f;
  c.noise_reduction.min_signal_remaining = 0.05f;
  c.pcan_gain_control.enable_pcan = 1;
  c.pcan_gain_control.strength = 0.95f;
  c.pcan_gain_control.offset = 80.0f;
  c.pcan_gain_control.gain_bits = 21;
  c.log_scale.enable_log = 1;
  c.log_scale.scale_shift = 6;
}

// Frontend output (about 0-670) to the model's int8 input: training divided by 25.6, then mapped 0-26 to -128-127.
int8_t quantize(uint16_t v) {
  const int32_t q = (static_cast<int32_t>(v) * 256 + 333) / 666 - 128;
  return static_cast<int8_t>(q < -128 ? -128 : q > 127 ? 127 : q);
}

using Resolver = tflite::MicroMutableOpResolver<21>;

bool add_ops(Resolver& r) {
  return r.AddCallOnce() == kTfLiteOk && r.AddVarHandle() == kTfLiteOk && r.AddReshape() == kTfLiteOk &&
         r.AddReadVariable() == kTfLiteOk && r.AddStridedSlice() == kTfLiteOk &&
         r.AddConcatenation() == kTfLiteOk && r.AddAssignVariable() == kTfLiteOk && r.AddConv2D() == kTfLiteOk &&
         r.AddMul() == kTfLiteOk && r.AddAdd() == kTfLiteOk && r.AddMean() == kTfLiteOk &&
         r.AddFullyConnected() == kTfLiteOk && r.AddLogistic() == kTfLiteOk && r.AddQuantize() == kTfLiteOk &&
         r.AddDepthwiseConv2D() == kTfLiteOk && r.AddAveragePool2D() == kTfLiteOk &&
         r.AddMaxPool2D() == kTfLiteOk && r.AddPad() == kTfLiteOk && r.AddPack() == kTfLiteOk &&
         r.AddSplitV() == kTfLiteOk;
}

struct Model {
  uint8_t* data = nullptr;
  uint8_t* arena = nullptr;
  tflite::MicroAllocator* allocator = nullptr;
  tflite::MicroResourceVariables* variables = nullptr;
  tflite::MicroInterpreter* interpreter = nullptr;
  int stride = 1;      // feature steps per inference (the model's input height)
  int filled = 0;      // steps in the input so far
  uint8_t cutoff = 0;  // 0-255
  int window = 1;
  uint8_t recent[16] = {};
  int recent_at = 0;
  uint32_t quiet_until = 0;  // no detection before this (warming up, or just after one)
};

// Two models on the same features: the wake word, and "stop" while something rings (timers, alarms).
SemaphoreHandle_t lock = nullptr;  // guards the models against load/unload while the task runs them
StreamBufferHandle_t samples = nullptr;
Resolver resolver;
Model* model = nullptr;
Model* stop_model = nullptr;
FrontendState frontend;
bool frontend_ok = false;
volatile bool listening = false, stop_listening = false;
volatile bool detected = false, stop_detected = false;
Stats stats_now;
uint64_t infer_us_total = 0;

// Wake clips (setClips) and tests (startTest): what the wake word heard lately, in a ring, and a copy of it at the
// last detection or test. Guarded by `lock`; PSRAM, allocated only while clips are on or a test needs them.
constexpr size_t RING_SAMPLES = TEST_SAMPLES;
int16_t* ring = nullptr;
size_t ring_at = 0, ring_filled = 0;
int16_t* clip = nullptr;
volatile size_t clip_len = 0;  // read without the lock by takeClip(), to skip it cheaply when there's nothing
Clip clip_info;
bool clips_wanted = false;  // setClips(true): keep the buffers after a test

// A test (startTest), armed by the task at the first audio after it starts, so it times what it actually hears.
volatile bool test_on = false;
bool test_armed = false;
uint32_t test_from = 0, test_end = 0;  // scores count from test_from (after the model's warm-up)
float test_peak = 0;

void ring_add(const int16_t* s, size_t n) {
  if (!ring) return;
  for (size_t i = 0; i < n; i++) {
    ring[ring_at] = s[i];
    ring_at = (ring_at + 1) % RING_SAMPLES;
  }
  ring_filled = min(ring_filled + n, RING_SAMPLES);
}

// The newest `max` samples of the ring into the clip.
void snapshot(size_t max, const Clip& info) {
  if (!ring || !clip) return;
  const size_t n = min(ring_filled, max);
  const size_t start = (ring_at + RING_SAMPLES - n) % RING_SAMPLES;
  for (size_t i = 0; i < n; i++) clip[i] = ring[(start + i) % RING_SAMPLES];
  clip_info = info;
  clip_info.samples = n;
  clip_len = n;
}

void free_buffers() {  // with the lock held (released while freeing, held again on return)
  int16_t* r = ring;
  int16_t* c = clip;
  ring = clip = nullptr;
  ring_at = ring_filled = clip_len = 0;
  xSemaphoreGive(lock);
  heap_caps_free(r);
  heap_caps_free(c);
  xSemaphoreTake(lock, portMAX_DELAY);
}

bool alloc_buffers() {  // without the lock; true if they're there
  if (ring) return true;
  auto* r = static_cast<int16_t*>(heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  auto* c = static_cast<int16_t*>(heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!r || !c) {
    heap_caps_free(r);
    heap_caps_free(c);
    return false;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (!ring) {
    std::swap(ring, r);
    std::swap(clip, c);
    ring_at = ring_filled = clip_len = 0;
  }
  xSemaphoreGive(lock);
  heap_caps_free(r);  // only if another call got there first
  heap_caps_free(c);
  return true;
}

void free_model(Model* m) {
  if (!m) return;
  delete m->interpreter;  // the allocator and variables live in the arena
  heap_caps_free(m->arena);
  heap_caps_free(m->data);
  delete m;
}

void reset_model(Model* m) {
  if (!m) return;
  m->filled = 0;
  memset(m->recent, 0, sizeof(m->recent));
  m->interpreter->Reset();
  m->quiet_until = millis() + WARM_UP_MS;
}

// Forgets everything heard so far. The streaming model keeps the last second or two of audio in its own variables,
// so without resetting those it would still "hear" the wake word that started the last conversation as soon as
// listening resumes, and wake again.
void reset_stream() {
  if (frontend_ok) FrontendReset(&frontend);
  ring_filled = 0;  // a clip starts where listening resumed
  reset_model(model);
  reset_model(stop_model);
  detected = false;
  stop_detected = false;
}

// One feature step into a model; runs it when a full stride has accumulated. True on a detection.
bool step(Model& m, const int8_t* features, bool count_stats) {
  TfLiteTensor* input = m.interpreter->input(0);
  memcpy(input->data.int8 + FEATURES * m.filled, features, FEATURES);
  if (++m.filled < m.stride) return false;
  m.filled = 0;
  const int64_t t0 = esp_timer_get_time();
  if (m.interpreter->Invoke() != kTfLiteOk) {
    LOGW(TAG, "inference failed");
    return false;
  }
  const uint8_t p = m.interpreter->output(0)->data.uint8[0];
  if (count_stats) {
    infer_us_total += esp_timer_get_time() - t0;
    stats_now.inferences++;
    if (p / 255.0f > stats_now.max_probability) stats_now.max_probability = p / 255.0f;
  }
  m.recent[m.recent_at] = p;
  m.recent_at = (m.recent_at + 1) % m.window;
  uint32_t sum = 0;
  for (int i = 0; i < m.window; i++) sum += m.recent[i];
  if (count_stats && test_on && test_armed && static_cast<int32_t>(millis() - test_from) >= 0) {
    test_peak = max(test_peak, static_cast<float>(sum) / (255.0f * m.window));
  }
  if (sum >= static_cast<uint32_t>(m.cutoff) * m.window && static_cast<int32_t>(millis() - m.quiet_until) >= 0) {
    LOGI(TAG, "%s (average %u%%)", count_stats ? "wake word" : "stop", static_cast<unsigned>(sum * 100 / (255 * m.window)));
    if (count_stats && test_on) {
      test_end = min(test_end, millis() + 500);  // heard: finish once the word has ended
    } else if (count_stats) {
      Clip info;
      info.probability = static_cast<float>(sum) / (255.0f * m.window);
      snapshot(CLIP_SAMPLES, info);
    }
    m.quiet_until = millis() + REFRACTORY_MS;
    memset(m.recent, 0, sizeof(m.recent));
    return true;
  }
  return false;
}

bool any_listening() { return (listening && model) || (stop_listening && stop_model); }

void task(void*) {
  static int16_t buf[160];  // 10 ms
  bool was_listening = false;
  for (;;) {
    const size_t n = xStreamBufferReceive(samples, buf, sizeof(buf), pdMS_TO_TICKS(100));
    const bool on = any_listening();
    const bool resumed = on && !was_listening;
    was_listening = on;
    if (!resumed && (!n || !on)) continue;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (resumed) reset_stream();  // a fresh start after a pause: no stale audio
    if (test_on && !test_armed && listening && model) {
      if (!resumed) reset_stream();  // a test hears only what's said after it starts
      test_armed = true;
      test_from = millis() + WARM_UP_MS;
      test_end = millis() + TEST_MS;
    }
    if (listening && model) ring_add(buf, n / sizeof(int16_t));
    if (frontend_ok) {
      const int16_t* p = buf;
      size_t left = n / sizeof(int16_t);
      while (left) {
        size_t read = 0;
        FrontendOutput out = FrontendProcessSamples(&frontend, p, left, &read);
        p += read;
        left -= read;
        if (out.size == FEATURES) {
          int8_t features[FEATURES];
          for (int i = 0; i < FEATURES; i++) features[i] = quantize(out.values[i]);
          if (listening && model && step(*model, features, true)) detected = true;
          if (stop_listening && stop_model && step(*stop_model, features, false)) stop_detected = true;
        }
        if (!read) break;
      }
    }
    if (test_on && test_armed && static_cast<int32_t>(millis() - test_end) >= 0) {
      Clip info;
      info.test = true;
      info.probability = test_peak;
      info.detected = model && test_peak * 255.0f >= model->cutoff;
      snapshot(TEST_SAMPLES, info);
      test_on = false;
      LOGI(TAG, "wake word test: %s, highest %u%%", info.detected ? "heard" : "not heard",
           static_cast<unsigned>(test_peak * 100));
    }
    xSemaphoreGive(lock);
  }
}

// A model from a .tflite file, ready to run; null (logged) if it can't be.
Model* build(const uint8_t* data, size_t size, float cutoff, int window, size_t arena) {
  auto* m = new Model();
  m->data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  arena += 8 * 1024;  // headroom for this TFLM build
  // PSRAM, not internal RAM: HTTPS needs the internal RAM (each TLS connection takes tens of KB), and with the arena
  // there no secure connection could be made at all. The model runs fast enough from PSRAM.
  m->arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, arena, MALLOC_CAP_SPIRAM));
  if (!m->data || !m->arena) {
    LOGE(TAG, "out of memory for the model");
    free_model(m);
    return nullptr;
  }
  memcpy(m->data, data, size);
  const tflite::Model* tf = tflite::GetModel(m->data);
  if (tf->version() != TFLITE_SCHEMA_VERSION) {
    LOGE(TAG, "model has schema %lu, not %d", tf->version(), TFLITE_SCHEMA_VERSION);
    free_model(m);
    return nullptr;
  }
  m->allocator = tflite::MicroAllocator::Create(m->arena, arena);
  m->variables = tflite::MicroResourceVariables::Create(m->allocator, RESOURCE_VARIABLES);
  m->interpreter = new tflite::MicroInterpreter(tf, resolver, m->allocator, m->variables);
  if (m->interpreter->AllocateTensors() != kTfLiteOk) {
    LOGE(TAG, "model doesn't fit in %u bytes", static_cast<unsigned>(arena));
    free_model(m);
    return nullptr;
  }
  TfLiteTensor* in = m->interpreter->input(0);
  TfLiteTensor* out = m->interpreter->output(0);
  if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[2] != FEATURES || out->type != kTfLiteUInt8) {
    LOGE(TAG, "not a streaming wake word model (input %d dims, type %d; output type %d)", in->dims->size, in->type,
         out->type);
    free_model(m);
    return nullptr;
  }
  m->stride = in->dims->data[1];
  m->cutoff = static_cast<uint8_t>(constrain(cutoff, 0.0f, 1.0f) * 255.0f);
  m->window = constrain(window, 1, 16);
  LOGI(TAG, "model loaded: %u bytes, stride %d, cutoff %.2f over %d, arena %u used of %u; internal RAM free %u "
       "(largest block %u)", static_cast<unsigned>(size), m->stride, cutoff, m->window,
       static_cast<unsigned>(m->interpreter->arena_used_bytes()), static_cast<unsigned>(arena),
       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
       static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  return m;
}

void swap_in(Model** slot, Model* m) {
  xSemaphoreTake(lock, portMAX_DELAY);
  Model* old = *slot;
  *slot = m;
  reset_model(m);
  xSemaphoreGive(lock);
  free_model(old);
}

}  // namespace

bool begin() {
  lock = xSemaphoreCreateMutex();
  samples = xStreamBufferCreateWithCaps(SAMPLE_BUFFER_BYTES, 2, MALLOC_CAP_SPIRAM);
  if (!lock || !samples || !add_ops(resolver)) {
    LOGE(TAG, "couldn't set up the wake word");
    return false;
  }
  FrontendConfig config;
  configure(config);
  frontend_ok = FrontendPopulateState(&config, &frontend, SAMPLE_RATE);
  if (!frontend_ok) LOGE(TAG, "audio frontend failed");
  // Its stack in PSRAM, to spare internal RAM: allowed because this task never writes to flash.
  if (xTaskCreatePinnedToCoreWithCaps(task, "wakeword", 8192, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    xTaskCreatePinnedToCore(task, "wakeword", 8192, nullptr, 3, nullptr, 0);
  }
  LOGI(TAG, "wake word ready; internal RAM free %u (largest block %u)",
       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
       static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  return frontend_ok;
}

bool load(const uint8_t* data, size_t size, float cutoff, int window, size_t arena) {
  Model* m = build(data, size, cutoff, window, arena);
  if (!m) return false;
  swap_in(&model, m);
  return true;
}

void unload() { swap_in(&model, nullptr); }

bool loaded() { return model != nullptr; }

bool loadStop(const uint8_t* data, size_t size, float cutoff, int window, size_t arena) {
  Model* m = build(data, size, cutoff, window, arena);
  if (!m) return false;
  swap_in(&stop_model, m);
  return true;
}

void unloadStop() { swap_in(&stop_model, nullptr); }

bool stopLoaded() { return stop_model != nullptr; }

void feed(const int16_t* pcm, size_t count) {
  if (!samples || !any_listening()) return;
  xStreamBufferSend(samples, pcm, count * sizeof(int16_t), 0);
}

void setListening(bool on) { listening = on; }

void setStopListening(bool on) { stop_listening = on; }

bool takeDetection() {
  if (!detected) return false;
  detected = false;
  return true;
}

void setClips(bool on) {
  if (!lock || on == clips_wanted) return;
  clips_wanted = on;
  if (on) {
    if (alloc_buffers()) LOGI(TAG, "wake clips on");
    else LOGW(TAG, "no memory for wake clips");
    return;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (ring && !test_on && !clip_len) free_buffers();  // otherwise once the test's clip is taken
  xSemaphoreGive(lock);
  LOGI(TAG, "wake clips off");
}

bool startTest() {
  if (!lock || !model || !alloc_buffers()) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  test_peak = 0;
  test_armed = false;
  clip_len = 0;  // an untaken wake clip would be overwritten anyway
  test_on = true;
  xSemaphoreGive(lock);
  return true;
}

bool testing() { return test_on; }

void cancelTest() {
  if (!lock) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  test_on = false;
  if (!clips_wanted && ring) free_buffers();
  xSemaphoreGive(lock);
}

Clip takeClip(int16_t* out) {
  Clip c;
  if (!lock || !clip_len) return c;  // called every loop: no lock unless there's a clip
  xSemaphoreTake(lock, portMAX_DELAY);
  if (clip && clip_len) {
    c = clip_info;
    c.samples = clip_len;
    memcpy(out, clip, c.samples * sizeof(int16_t));
    clip_len = 0;
  }
  if (!clips_wanted && !test_on && ring) free_buffers();  // kept only for the test
  xSemaphoreGive(lock);
  return c;
}

bool takeStop() {
  if (!stop_detected) return false;
  stop_detected = false;
  return true;
}

Stats stats() {
  Stats s = stats_now;
  s.avg_infer_us = s.inferences ? static_cast<uint32_t>(infer_us_total / s.inferences) : 0;
  stats_now = Stats();
  infer_us_total = 0;
  return s;
}

}  // namespace wakeword
