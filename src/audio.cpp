#include "audio.h"

#include <Wire.h>
#include <driver/i2s_std.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include "board.h"
#include "logbuf.h"

namespace audio {
namespace {

constexpr const char* TAG = "audio";
constexpr uint8_t ES8311 = 0x18;
constexpr size_t CHUNK = 256;            // samples per I2S write/read
constexpr uint32_t AMP_OFF_AFTER_MS = 200;  // after the last sound, so the DMA buffers drain first

i2s_chan_handle_t tx = nullptr, rx = nullptr;
QueueHandle_t queue = nullptr;
SemaphoreHandle_t rec_mutex = nullptr;
volatile int current_volume = 70;
volatile bool mic_muted = false;
bool ok = false;

// Codec registers

bool wr(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ES8311);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

int rd(uint8_t reg) {
  Wire.beginTransmission(ES8311);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(ES8311, static_cast<uint8_t>(1)) != 1) return -1;
  return Wire.read();
}

// ES8311 as I2S slave, MCLK = 256 x 16 kHz from the ESP32, 16-bit I2S both ways, analog mic.
// Register values follow Espressif's es8311 driver (coefficients for 4.096 MHz MCLK / 16 kHz).
bool codec_init() {
  if (rd(0xFD) < 0) return false;  // chip ID register: just checks the codec answers
  const uint8_t init[][2] = {
      {0x00, 0x1F}, {0x45, 0x00},                                      // reset
      {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
      {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
      {0x00, 0x80},                                                    // power on, slave mode
      {0x01, 0x3F},                                                    // all clocks on, MCLK from the pin
      {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10}, {0x04, 0x10},          // dividers for 16 kHz
      {0x07, 0x00}, {0x08, 0xFF}, {0x06, 0x03},
      {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A},                        // ADC high-pass (removes DC)
      {0x09, 0x0C}, {0x0A, 0x0C},                                      // DAC/ADC: I2S, 16-bit
      {0x17, 0xBF},                                                    // ADC volume 0 dB
      {0x0E, 0x02}, {0x12, 0x00},                                      // ADC modulator + DAC on
      {0x14, 0x1A},                                                    // analog mic in, PGA +30 dB
      {0x0D, 0x01}, {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00},
  };
  for (const auto& r : init) {
    if (!wr(r[0], r[1])) return false;
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  return true;
}

void apply_volume() {
  const int v = current_volume;
  // DAC volume: 0xBF = 0 dB, 0.5 dB per step. 100 -> 0 dB, 1 -> -49.5 dB, 0 -> fully muted.
  wr(0x32, v <= 0 ? 0x00 : 0xBF - (100 - v));
}

void apply_mic() {
  wr(0x17, mic_muted ? 0x00 : 0xBF);  // ADC volume; the capture task also drops samples while muted
}

// I2S, full duplex on one port

bool i2s_init() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 6;
  chan.dma_frame_num = 240;
  chan.auto_clear = true;  // send silence when nothing is queued, instead of repeating the last buffer
  if (i2s_new_channel(&chan, &tx, &rx) != ESP_OK) return false;
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = static_cast<gpio_num_t>(pins::I2S_MCLK),
              .bclk = static_cast<gpio_num_t>(pins::I2S_BCLK),
              .ws = static_cast<gpio_num_t>(pins::I2S_WS),
              .dout = static_cast<gpio_num_t>(pins::I2S_DOUT),
              .din = static_cast<gpio_num_t>(pins::I2S_DIN),
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  // Mono with both slots enabled reads left and right interleaved (twice the samples): capture the left slot only.
  i2s_std_config_t in = std;
  in.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  return i2s_channel_init_std_mode(tx, &std) == ESP_OK && i2s_channel_init_std_mode(rx, &in) == ESP_OK &&
         i2s_channel_enable(tx) == ESP_OK && i2s_channel_enable(rx) == ESP_OK;
}

// Sounds: short note sequences, synthesized as a soft bell (sine plus a little second harmonic).

struct Note {
  uint16_t hz;  // 0 = rest
  uint16_t ms;
  uint8_t level;  // percent of full scale
};

constexpr Note BOOT[] = {{523, 90, 45}, {659, 90, 45}, {784, 90, 45}, {1047, 320, 50}, {0, 0, 0}};
constexpr Note ACTIVATE[] = {{784, 70, 50}, {1175, 180, 55}, {0, 0, 0}};
constexpr Note DONE[] = {{1175, 70, 45}, {784, 150, 40}, {0, 0, 0}};
constexpr Note CANCEL[] = {{659, 90, 35}, {523, 180, 30}, {0, 0, 0}};
constexpr Note ERROR_[] = {{220, 140, 50}, {0, 60, 0}, {208, 260, 50}, {0, 0, 0}};
constexpr Note MUTE_ON[] = {{659, 90, 45}, {440, 220, 45}, {0, 0, 0}};
constexpr Note MUTE_OFF[] = {{440, 90, 45}, {659, 220, 45}, {0, 0, 0}};
constexpr Note TICK[] = {{1500, 22, 25}, {0, 0, 0}};

const Note* notes_for(Sound s) {
  switch (s) {
    case Sound::Boot: return BOOT;
    case Sound::Activate: return ACTIVATE;
    case Sound::Done: return DONE;
    case Sound::Cancel: return CANCEL;
    case Sound::Error: return ERROR_;
    case Sound::MuteOn: return MUTE_ON;
    case Sound::MuteOff: return MUTE_OFF;
    case Sound::Tick: return TICK;
  }
  return TICK;
}

void write_samples(const int16_t* data, size_t count) {
  size_t written;
  i2s_channel_write(tx, data, count * sizeof(int16_t), &written, pdMS_TO_TICKS(500));
}

void render(const Note* notes) {
  static int16_t buf[CHUNK];
  for (const Note* n = notes; n->ms; n++) {
    const uint32_t total = SAMPLE_RATE * n->ms / 1000;
    const float amp = 32767.0f * n->level / 100.0f;
    const float w = 2.0f * PI * n->hz / SAMPLE_RATE;
    const float attack = SAMPLE_RATE * 0.004f, tau = total * 0.45f;
    uint32_t i = 0;
    while (i < total) {
      const size_t m = min<uint32_t>(CHUNK, total - i);
      for (size_t k = 0; k < m; k++, i++) {
        if (!n->hz) {
          buf[k] = 0;
          continue;
        }
        const float env = min(1.0f, i / attack) * expf(-static_cast<float>(i) / tau) *
                          min(1.0f, (total - i) / attack);  // short release so notes don't click
        buf[k] = static_cast<int16_t>(amp * env * (0.85f * sinf(w * i) + 0.15f * sinf(2 * w * i)));
      }
      write_samples(buf, m);
    }
  }
}

// Speech from Kotodama: PCM written by the main task into a PSRAM stream buffer, played by the play task.
constexpr size_t SPEECH_BUFFER_BYTES = 512 * 1024;  // 16 s; Kotodama stays at most 3 s ahead
StreamBufferHandle_t speech = nullptr;
volatile bool speech_on = false, speech_ending = false;

void amp(bool on) {
  static bool is_on = false;
  if (on == is_on) return;
  digitalWrite(pins::AMP_EN, on ? HIGH : LOW);
  is_on = on;
  if (on) delay(5);
}

void play_task(void*) {
  static int16_t buf[CHUNK];
  for (;;) {
    Sound s;
    if (xQueueReceive(queue, &s, speech_on ? 0 : pdMS_TO_TICKS(AMP_OFF_AFTER_MS)) == pdTRUE) {
      if (current_volume <= 0) continue;
      amp(true);
      render(notes_for(s));
      continue;
    }
    if (speech_on) {
      const size_t n = xStreamBufferReceive(speech, buf, sizeof(buf), pdMS_TO_TICKS(20));
      if (n) {
        amp(true);
        write_samples(buf, n / sizeof(int16_t));
      } else if (speech_ending) {
        speech_on = false;  // everything sent has been played
      }
      continue;
    }
    amp(false);
  }
}

// Microphone: while listening, captured audio goes into a stream buffer that the main task drains and sends.

constexpr size_t MIC_BUFFER_BYTES = 64 * 1024;  // 2 s of slack if Wi-Fi stalls
StreamBufferHandle_t mic = nullptr;

struct Listen {
  size_t samples = 0, max_samples = 0;
  size_t silence_limit = 0;  // stop after this many samples without speech
  size_t quiet = 0;          // samples since speech was last heard (or since the start)
  bool speech = false;
  int16_t peak = 0;
  size_t dropped = 0;  // samples lost because the buffer was full
};
Listen listen_state;
volatile bool listen_on = false;

// Voice activity: a chunk is speech when its level is well above the background noise, which is tracked all the
// time (also between sessions) as a slowly rising, quickly falling floor.
constexpr float SPEECH_RATIO = 3.0f;
constexpr float MIN_SPEECH_LEVEL = 250.0f;  // mean absolute amplitude, out of 32767
float noise_floor = 200.0f;

bool is_speech(const int16_t* s, size_t n) {
  float sum = 0;
  for (size_t i = 0; i < n; i++) sum += abs(s[i]);
  const float level = sum / n;
  const bool speech = level > noise_floor * SPEECH_RATIO && level > MIN_SPEECH_LEVEL;
  if (level < noise_floor) noise_floor += (level - noise_floor) * 0.1f;
  else if (!speech) noise_floor += (level - noise_floor) * 0.01f;
  noise_floor = max(noise_floor, 20.0f);
  return speech;
}

void capture_task(void*) {
  static int16_t buf[CHUNK];
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200)) != ESP_OK || !got) continue;
    const size_t n = got / sizeof(int16_t);
    const bool speech = !mic_muted && is_speech(buf, n);
    xSemaphoreTake(rec_mutex, portMAX_DELAY);
    if (listen_on) {
      Listen& l = listen_state;
      if (mic_muted) {
        listen_on = false;
      } else {
        if (speech) {
          l.speech = true;
          l.quiet = 0;
        } else {
          l.quiet += n;
        }
        for (size_t i = 0; i < n; i++) {
          const int16_t a = buf[i] == INT16_MIN ? INT16_MAX : abs(buf[i]);
          if (a > l.peak) l.peak = a;
        }
        const size_t sent = xStreamBufferSend(mic, buf, got, 0);
        l.dropped += (got - sent) / sizeof(int16_t);
        l.samples += n;
        if (l.samples >= l.max_samples || l.quiet >= l.silence_limit) listen_on = false;
      }
    }
    xSemaphoreGive(rec_mutex);
  }
}

StreamBufferHandle_t psram_stream(size_t bytes) {
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(bytes + 1, MALLOC_CAP_SPIRAM));
  auto* control = static_cast<StaticStreamBuffer_t*>(heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL));
  return storage && control ? xStreamBufferCreateStatic(bytes, 1, storage, control) : nullptr;
}

}  // namespace

bool begin() {
  pinMode(pins::AMP_EN, OUTPUT);
  digitalWrite(pins::AMP_EN, LOW);
  Wire.begin(pins::CODEC_SDA, pins::CODEC_SCL, 100000);
  rec_mutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(8, sizeof(Sound));
  speech = psram_stream(SPEECH_BUFFER_BYTES);
  mic = psram_stream(MIC_BUFFER_BYTES);
  if (!speech || !mic) {
    LOGE(TAG, "out of memory for audio buffers");
    return false;
  }
  // I2S first: the codec needs MCLK running to accept its configuration.
  if (!i2s_init()) {
    LOGE(TAG, "I2S init failed");
    return false;
  }
  if (!codec_init()) {
    LOGE(TAG, "ES8311 codec not responding");
    return false;
  }
  apply_volume();
  apply_mic();
  xTaskCreatePinnedToCore(play_task, "audio_out", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(capture_task, "audio_in", 4096, nullptr, 5, nullptr, 1);
  ok = true;
  LOGI(TAG, "ES8311 ready, %d Hz", SAMPLE_RATE);
  return true;
}

void setVolume(int percent) {
  current_volume = constrain(percent, 0, 100);
  if (ok) apply_volume();
}

int volume() { return current_volume; }

void play(Sound sound) {
  if (!ok) return;
  if (sound == Sound::Tick && uxQueueMessagesWaiting(queue)) return;
  xQueueSend(queue, &sound, 0);
}

void setMicMuted(bool muted) {
  mic_muted = muted;
  if (ok) apply_mic();
}

bool micMuted() { return mic_muted; }

bool startListening(uint32_t max_ms, uint32_t silence_ms) {
  if (!ok || mic_muted) return false;
  xSemaphoreTake(rec_mutex, portMAX_DELAY);
  xStreamBufferReset(mic);
  listen_state = {};
  listen_state.max_samples = SAMPLE_RATE / 1000 * max_ms;
  listen_state.silence_limit = SAMPLE_RATE / 1000 * silence_ms;
  listen_on = true;
  xSemaphoreGive(rec_mutex);
  return true;
}

void stopListening() { listen_on = false; }

bool listening() { return listen_on; }

size_t readMic(uint8_t* dst, size_t max_bytes) { return mic ? xStreamBufferReceive(mic, dst, max_bytes & ~1u, 0) : 0; }

ListenInfo listenInfo() {
  xSemaphoreTake(rec_mutex, portMAX_DELAY);
  ListenInfo info;
  info.duration_ms = listen_state.samples * 1000 / SAMPLE_RATE;
  info.peak_percent = listen_state.peak * 100 / 32767;
  info.speech = listen_state.speech;
  info.noise_floor = static_cast<int>(noise_floor);
  info.dropped_ms = listen_state.dropped * 1000 / SAMPLE_RATE;
  xSemaphoreGive(rec_mutex);
  return info;
}

void startSpeech() {
  if (!ok) return;
  speech_on = false;
  for (int i = 0; i < 10 && xStreamBufferReset(speech) != pdPASS; i++) delay(10);  // the play task may still be waiting on it
  speech_ending = false;
  speech_on = true;
}

size_t writeSpeech(const uint8_t* pcm, size_t bytes) {
  return ok && speech_on ? xStreamBufferSend(speech, pcm, bytes & ~1u, 0) : 0;
}

void endSpeech() { speech_ending = true; }

void stopSpeech() {
  speech_on = false;
  speech_ending = false;
}

bool speaking() { return speech_on; }

}  // namespace audio
