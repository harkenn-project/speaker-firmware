#include "wakeword.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/stream_buffer.h>

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
};

SemaphoreHandle_t lock = nullptr;  // guards `model` against load/unload while the task runs it
StreamBufferHandle_t samples = nullptr;
Resolver resolver;
Model* model = nullptr;
FrontendState frontend;
bool frontend_ok = false;
volatile bool listening = false;
volatile bool detected = false;
uint32_t quiet_until = 0;
Stats stats_now;
uint64_t infer_us_total = 0;

void free_model(Model* m) {
  if (!m) return;
  delete m->interpreter;  // the allocator and variables live in the arena
  heap_caps_free(m->arena);
  heap_caps_free(m->data);
  delete m;
}

void reset_stream() {
  if (frontend_ok) FrontendReset(&frontend);
  if (model) {
    model->filled = 0;
    memset(model->recent, 0, sizeof(model->recent));
  }
}

// One feature step into the model; runs it when a full stride has accumulated.
void step(const int8_t* features) {
  Model& m = *model;
  TfLiteTensor* input = m.interpreter->input(0);
  memcpy(input->data.int8 + FEATURES * m.filled, features, FEATURES);
  if (++m.filled < m.stride) return;
  m.filled = 0;
  const int64_t t0 = esp_timer_get_time();
  if (m.interpreter->Invoke() != kTfLiteOk) {
    LOGW(TAG, "inference failed");
    return;
  }
  infer_us_total += esp_timer_get_time() - t0;
  stats_now.inferences++;
  const uint8_t p = m.interpreter->output(0)->data.uint8[0];
  if (p / 255.0f > stats_now.max_probability) stats_now.max_probability = p / 255.0f;
  m.recent[m.recent_at] = p;
  m.recent_at = (m.recent_at + 1) % m.window;
  uint32_t sum = 0;
  for (int i = 0; i < m.window; i++) sum += m.recent[i];
  if (sum >= static_cast<uint32_t>(m.cutoff) * m.window && static_cast<int32_t>(millis() - quiet_until) >= 0) {
    LOGI(TAG, "wake word (average %u%%)", static_cast<unsigned>(sum * 100 / (255 * m.window)));
    detected = true;
    quiet_until = millis() + REFRACTORY_MS;
    memset(m.recent, 0, sizeof(m.recent));
  }
}

void task(void*) {
  static int16_t buf[160];  // 10 ms
  bool was_listening = false;
  for (;;) {
    const size_t n = xStreamBufferReceive(samples, buf, sizeof(buf), pdMS_TO_TICKS(100));
    const bool on = listening;
    if (on && !was_listening) reset_stream();  // a fresh start after a pause: no stale audio
    was_listening = on;
    if (!n || !on) continue;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (model && frontend_ok) {
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
          step(features);
        }
        if (!read) break;
      }
    }
    xSemaphoreGive(lock);
  }
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
  xTaskCreatePinnedToCore(task, "wakeword", 8192, nullptr, 3, nullptr, 0);
  return frontend_ok;
}

bool load(const uint8_t* data, size_t size, float cutoff, int window, size_t arena) {
  auto* m = new Model();
  m->data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  arena += 8 * 1024;  // headroom for this TFLM build
  m->arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, arena, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!m->arena) m->arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, arena, MALLOC_CAP_SPIRAM));
  if (!m->data || !m->arena) {
    LOGE(TAG, "out of memory for the wake word model");
    free_model(m);
    return false;
  }
  memcpy(m->data, data, size);
  const tflite::Model* tf = tflite::GetModel(m->data);
  if (tf->version() != TFLITE_SCHEMA_VERSION) {
    LOGE(TAG, "wake word model has schema %lu, not %d", tf->version(), TFLITE_SCHEMA_VERSION);
    free_model(m);
    return false;
  }
  m->allocator = tflite::MicroAllocator::Create(m->arena, arena);
  m->variables = tflite::MicroResourceVariables::Create(m->allocator, RESOURCE_VARIABLES);
  m->interpreter = new tflite::MicroInterpreter(tf, resolver, m->allocator, m->variables);
  if (m->interpreter->AllocateTensors() != kTfLiteOk) {
    LOGE(TAG, "wake word model doesn't fit in %u bytes", static_cast<unsigned>(arena));
    free_model(m);
    return false;
  }
  TfLiteTensor* in = m->interpreter->input(0);
  TfLiteTensor* out = m->interpreter->output(0);
  if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[2] != FEATURES || out->type != kTfLiteUInt8) {
    LOGE(TAG, "not a streaming wake word model (input %d dims, type %d; output type %d)", in->dims->size, in->type,
         out->type);
    free_model(m);
    return false;
  }
  m->stride = in->dims->data[1];
  m->cutoff = static_cast<uint8_t>(constrain(cutoff, 0.0f, 1.0f) * 255.0f);
  m->window = constrain(window, 1, 16);
  xSemaphoreTake(lock, portMAX_DELAY);
  Model* old = model;
  model = m;
  reset_stream();
  xSemaphoreGive(lock);
  free_model(old);
  LOGI(TAG, "wake word model loaded: %u bytes, stride %d, cutoff %.2f over %d, arena %u used of %u",
       static_cast<unsigned>(size), m->stride, cutoff, m->window,
       static_cast<unsigned>(m->interpreter->arena_used_bytes()), static_cast<unsigned>(arena));
  return true;
}

void unload() {
  xSemaphoreTake(lock, portMAX_DELAY);
  Model* old = model;
  model = nullptr;
  xSemaphoreGive(lock);
  free_model(old);
}

bool loaded() { return model != nullptr; }

void feed(const int16_t* pcm, size_t count) {
  if (!samples || !listening || !model) return;
  xStreamBufferSend(samples, pcm, count * sizeof(int16_t), 0);
}

void setListening(bool on) { listening = on; }

bool takeDetection() {
  if (!detected) return false;
  detected = false;
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
