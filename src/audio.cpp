#include "audio.h"

#include <LittleFS.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include <esp_random.h>

#include "board.h"
#include "logbuf.h"
#include "wakeword.h"

namespace audio {
namespace {

constexpr const char* TAG = "audio";
constexpr uint8_t ES8311 = 0x18;
constexpr size_t CHUNK = 256;            // 16 kHz samples per write/read (16 ms)
constexpr int RATIO = HW_RATE / SAMPLE_RATE;
int ratio = RATIO;  // codec rate / 16 kHz: 3, or 1 when it runs at 16 kHz (no wake word arbitration; begin())
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

// ES8311 as I2S slave, MCLK = 256 x 48 kHz from the ESP32, 16-bit I2S both ways, analog mic.
// Register values follow Espressif's es8311 driver (coefficients for 12.288 MHz MCLK / 48 kHz: the same dividers as
// for 4.096 MHz / 16 kHz, since only the ratio matters).
bool codec_init() {
  if (rd(0xFD) < 0) return false;  // chip ID register: just checks the codec answers
  const uint8_t init[][2] = {
      {0x00, 0x1F}, {0x45, 0x00},                                      // reset
      {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
      {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
      {0x00, 0x80},                                                    // power on, slave mode
      {0x01, 0x3F},                                                    // all clocks on, MCLK from the pin
      {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10}, {0x04, 0x10},          // dividers for MCLK = 256 fs
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
  chan.dma_frame_num = 480;  // 10 ms at 48 kHz
  chan.auto_clear = true;  // send silence when nothing is queued, instead of repeating the last buffer
  if (i2s_new_channel(&chan, &tx, &rx) != ESP_OK) return false;
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(static_cast<uint32_t>(SAMPLE_RATE * ratio)),
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

// 16 <-> 48 kHz: one low-pass FIR (windowed sinc, cut at 7 kHz) used to decimate the microphone and to interpolate
// everything played, so nothing above 8 kHz goes to the rest of the speaker and nothing it plays reaches the
// claim tone's band.

constexpr int TAPS = 96;              // 32 per phase
constexpr int PHASE_TAPS = TAPS / RATIO;
float fir[TAPS];

void fir_init() {
  const float fc = 7000.0f / HW_RATE, mid = (TAPS - 1) / 2.0f;
  float sum = 0;
  for (int k = 0; k < TAPS; k++) {
    const float x = k - mid;
    const float sinc = x == 0 ? 2 * fc : sinf(2 * PI * fc * x) / (PI * x);
    const float blackman = 0.42f - 0.5f * cosf(2 * PI * k / (TAPS - 1)) + 0.08f * cosf(4 * PI * k / (TAPS - 1));
    fir[k] = sinc * blackman;
    sum += fir[k];
  }
  for (float& h : fir) h /= sum;  // unity gain at DC
}

int16_t clip16(float v) { return static_cast<int16_t>(constrain(v, -32768.0f, 32767.0f)); }

// 48 kHz in (n samples, a multiple of RATIO), 16 kHz out.
void decimate(const int16_t* in, size_t n, int16_t* out) {
  static float ext[TAPS - 1 + CHUNK * RATIO];  // the last TAPS-1 inputs, then this block
  if (ratio == 1) {
    memcpy(out, in, n * sizeof(int16_t));
    return;
  }
  for (size_t i = 0; i < n; i++) ext[TAPS - 1 + i] = in[i];
  for (size_t j = 0; j < n / RATIO; j++) {
    const float* x = ext + TAPS - 1 + RATIO * j + RATIO - 1;
    float y = 0;
    for (int k = 0; k < TAPS; k++) y += fir[k] * x[-k];
    out[j] = clip16(y);
  }
  memmove(ext, ext + n, (TAPS - 1) * sizeof(float));
}

// 16 kHz in (n samples), 48 kHz out (n * RATIO). Only the play task calls it.
void interpolate(const int16_t* in, size_t n, int16_t* out) {
  static float ext[PHASE_TAPS - 1 + CHUNK];
  if (ratio == 1) {
    memcpy(out, in, n * sizeof(int16_t));
    return;
  }
  for (size_t i = 0; i < n; i++) ext[PHASE_TAPS - 1 + i] = in[i];
  for (size_t i = 0; i < n; i++) {
    const float* x = ext + PHASE_TAPS - 1 + i;
    for (int ph = 0; ph < RATIO; ph++) {
      float y = 0;
      for (int j = 0; j < PHASE_TAPS; j++) y += fir[ph + RATIO * j] * x[-j];
      out[RATIO * i + ph] = clip16(RATIO * y);
    }
  }
  memmove(ext, ext + n, (PHASE_TAPS - 1) * sizeof(float));
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
constexpr Note LISTEN[] = {{988, 45, 28}, {1319, 80, 30}, {0, 0, 0}};
constexpr Note SHUTDOWN[] = {{1047, 90, 45}, {784, 90, 45}, {659, 90, 45}, {523, 320, 45}, {0, 0, 0}};
// A phone-like trill, played every RING_EVERY_MS while a speaker rings (app.cpp).
constexpr Note RING[] = {{1047, 55, 55}, {1319, 55, 55}, {1047, 55, 55}, {1319, 55, 55}, {1047, 55, 55},
                         {1319, 55, 55}, {1047, 55, 55}, {1319, 55, 55}, {0, 180, 0}, {1047, 55, 55},
                         {1319, 55, 55}, {1047, 55, 55}, {1319, 55, 55}, {1047, 55, 55}, {1319, 55, 55},
                         {1047, 55, 55}, {1319, 55, 55}, {0, 0, 0}};

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
    case Sound::Ring: return RING;
    case Sound::Shutdown: return SHUTDOWN;
    case Sound::Listen: return LISTEN;
    case Sound::Claim: return TICK;  // played by render_claim()
  }
  return TICK;
}

void write_raw(const int16_t* data, size_t count) {  // 48 kHz
  size_t written;
  i2s_channel_write(tx, data, count * sizeof(int16_t), &written, pdMS_TO_TICKS(500));
}

void write_samples(const int16_t* data, size_t count) {  // 16 kHz
  static int16_t out[CHUNK * RATIO];
  for (size_t i = 0; i < count; i += CHUNK) {
    const size_t m = min(CHUNK, count - i);
    interpolate(data + i, m, out);
    write_raw(out, m * ratio);
  }
}

// Our claim (audio.h): three tones with soft edges (no clicks), at full level whatever the volume.
volatile uint32_t claiming_until = 0;  // the capture task ignores what it hears of our own claim
volatile int claim_level = 0;          // for the queued Sound::Claim

uint32_t claim_start_ms(int tone, int level) {
  return tone == 0 ? 0 : tone == 1 ? CLAIM_AB_MS : CLAIM_AB_MS + CLAIM_BC_MS + level * CLAIM_SLOT_MS;
}

void render_claim(int level) {
  static int16_t buf[CHUNK * RATIO];
  const uint32_t tone_len = HW_RATE * CLAIM_TONE_MS / 1000, ramp = HW_RATE * 5 / 1000;
  uint32_t start[3];
  for (int t = 0; t < 3; t++) start[t] = HW_RATE * claim_start_ms(t, level) / 1000;
  const uint32_t total = start[2] + tone_len;
  claiming_until = millis() + total * 1000 / HW_RATE + 150;
  wr(0x32, 0xBF);  // DAC at 0 dB for the tones
  for (uint32_t i = 0; i < total;) {
    const size_t m = min<uint32_t>(CHUNK * RATIO, total - i);
    for (size_t k = 0; k < m; k++, i++) {
      float v = 0;
      for (int t = 0; t < 3; t++) {
        if (i < start[t] || i >= start[t] + tone_len) continue;
        const uint32_t at = i - start[t], edge = min(at, tone_len - 1 - at);
        const float env = edge >= ramp ? 1.0f : 0.5f - 0.5f * cosf(PI * edge / ramp);
        v = 23000.0f * env * sinf(2.0f * PI * CLAIM_HZ[t] * at / HW_RATE);
      }
      buf[k] = static_cast<int16_t>(v);
    }
    write_raw(buf, m);
  }
  claiming_until = millis() + 150;  // what's still in the DMA buffers, and the echo
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

// Speech from Harkenn: PCM written by the main task into a PSRAM stream buffer, played by the play task.
constexpr size_t SPEECH_BUFFER_BYTES = 512 * 1024;  // 16 s; Harkenn stays at most 3 s ahead
StreamBufferHandle_t speech = nullptr;
volatile bool speech_on = false, speech_ending = false;

void amp(bool on) {
  static bool is_on = false;
  if (on == is_on) return;
  digitalWrite(pins::AMP_EN, on ? HIGH : LOW);
  is_on = on;
  if (on) delay(5);
}

// The sound pack's clips, in flash: /<slot>.pcm, and /<slot>.url with where it came from.
bool fs_ok = false;
volatile bool sound_playing = false;

const char* slot_for(Sound s) {
  switch (s) {
    case Sound::Boot: return "boot";
    case Sound::Activate: return "wake";
    case Sound::Done: return "done";
    case Sound::Error: return "error";
    case Sound::Shutdown: return "shutdown";
    default: return nullptr;
  }
}

String clip_path(const char* slot) { return String("/") + slot + ".pcm"; }

// Plays the slot's clip if there is one (true), including a silent one.
bool play_clip(Sound s) {
  const char* slot = slot_for(s);
  if (!fs_ok || !slot) return false;
  File f = LittleFS.open(clip_path(slot), "r");
  if (!f) return false;
  static int16_t buf[CHUNK];
  size_t n;
  while ((n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf))) > 0) write_samples(buf, n / sizeof(int16_t));
  f.close();
  return true;
}

void play_task(void*) {
  static int16_t buf[CHUNK];
  for (;;) {
    Sound s;
    if (xQueueReceive(queue, &s, speech_on ? 0 : pdMS_TO_TICKS(AMP_OFF_AFTER_MS)) == pdTRUE) {
      if (s == Sound::Claim) {  // inaudible: played whatever the volume
        sound_playing = true;
        amp(true);
        render_claim(claim_level);
        apply_volume();
        sound_playing = false;
        continue;
      }
      if (current_volume <= 0) continue;
      sound_playing = true;
      amp(true);
      if (!play_clip(s)) render(notes_for(s));
      sound_playing = false;
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

// The microphone's level since micStats() was last called.
float stats_sum = 0, stats_max = 0;
uint32_t stats_count = 0;

// The level of each chunk over the last 1.5 s, for recentLevelDb().
constexpr size_t LEVELS = SAMPLE_RATE * 3 / 2 / CHUNK;
uint16_t levels[LEVELS];
size_t level_at = 0;

bool is_speech(const int16_t* s, size_t n) {
  float sum = 0;
  for (size_t i = 0; i < n; i++) sum += abs(s[i]);
  const float level = sum / n;
  levels[level_at] = static_cast<uint16_t>(level);
  level_at = (level_at + 1) % LEVELS;
  stats_sum += level;
  stats_count++;
  if (level > stats_max) stats_max = level;
  const bool speech = level > noise_floor * SPEECH_RATIO && level > MIN_SPEECH_LEVEL;
  if (level < noise_floor) noise_floor += (level - noise_floor) * 0.1f;
  else if (!speech) noise_floor += (level - noise_floor) * 0.01f;
  noise_floor = max(noise_floor, 20.0f);
  return speech;
}

// Listening for other speakers' claims in the 48 kHz audio: Goertzel filters at the three tones and at a reference
// frequency below them, over 5 ms blocks. A tone starts when it's the strongest of the three, well above the
// reference (broadband noise) and a minimum level, for 3 blocks in a row; a claim is the three starting in order, the
// second CLAIM_AB_MS after the first, the third CLAIM_BC_MS plus its level's slots after that.
constexpr int BLOCK_MS = 5;
constexpr int TONE_BLOCK = HW_RATE * BLOCK_MS / 1000;
constexpr int REF_HZ = 17000;
constexpr float TONE_MIN_AMP = 30.0f;  // out of 32767
constexpr float TONE_OVER_REF = 4.0f;
constexpr int ONSET_BLOCKS = 3;
constexpr int32_t SLACK_MS = 12;  // timing tolerance
portMUX_TYPE heard_lock = portMUX_INITIALIZER_UNLOCKED;
HeardClaim heard;

struct Goertzel {
  float coeff, s1 = 0, s2 = 0;
  explicit Goertzel(float hz) : coeff(2.0f * cosf(2.0f * PI * hz / HW_RATE)) {}
  void add(float x) {
    const float s0 = x + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  float amplitude() {  // of a sine at this frequency over the block, then start over
    const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    s1 = s2 = 0;
    return 2.0f * sqrtf(max(power, 0.0f)) / TONE_BLOCK;
  }
};

// A tone started (at `t` ms, on the block clock): advance through the claim's three tones.
void tone_started(int tone, int32_t t, float amp) {
  static int stage = 0;  // tones of a claim seen so far
  static int32_t t_a = 0, t_b = 0;
  static float strength = 0;
  if (stage && t - (stage == 1 ? t_a : t_b) > 400) stage = 0;  // too long since: start over
  if (tone == 0) {
    stage = 1;
    t_a = t;
    strength = amp;
  } else if (tone == 1 && stage == 1 && abs(t - t_a - static_cast<int32_t>(CLAIM_AB_MS)) <= SLACK_MS) {
    stage = 2;
    t_b = t;
  } else if (tone == 2 && stage == 2) {
    const int32_t delay = t - t_b - CLAIM_BC_MS;
    const int level = (delay + static_cast<int32_t>(CLAIM_SLOT_MS) / 2) / static_cast<int32_t>(CLAIM_SLOT_MS);
    if (delay >= -SLACK_MS && level < CLAIM_LEVELS) {
      HeardClaim c;
      c.at = millis() | 1;  // never 0
      c.level = max(level, 0);
      c.strength = static_cast<int>(strength);
      portENTER_CRITICAL(&heard_lock);
      heard = c;
      portEXIT_CRITICAL(&heard_lock);
    }
    stage = 0;
  } else {
    stage = 0;  // out of order: not a claim
  }
}

void listen_for_claims(const int16_t* raw, size_t n) {
  static Goertzel tones[3] = {Goertzel(CLAIM_HZ[0]), Goertzel(CLAIM_HZ[1]), Goertzel(CLAIM_HZ[2])};
  static Goertzel ref(REF_HZ);
  static int count = 0;
  static int32_t block = 0;
  static int run[3] = {0, 0, 0};
  for (size_t i = 0; i < n; i++) {
    for (Goertzel& g : tones) g.add(raw[i]);
    ref.add(raw[i]);
    if (++count < TONE_BLOCK) continue;
    count = 0;
    block++;
    float amp[3];
    int best = 0;
    for (int t = 0; t < 3; t++) {
      amp[t] = tones[t].amplitude();
      if (amp[t] > amp[best]) best = t;
    }
    const float r = ref.amplitude();
    const bool ours = static_cast<int32_t>(claiming_until - millis()) > 0;
    const bool present = !ours && amp[best] > TONE_MIN_AMP && amp[best] > TONE_OVER_REF * r;
    for (int t = 0; t < 3; t++) run[t] = present && t == best ? run[t] + 1 : 0;
    if (present && run[best] == ONSET_BLOCKS) tone_started(best, (block - ONSET_BLOCKS + 1) * BLOCK_MS, amp[best]);
  }
}

void capture_task(void*) {
  static int16_t raw[CHUNK * RATIO];
  static int16_t buf[CHUNK];
  for (;;) {
    size_t got_raw = 0;
    if (i2s_channel_read(rx, raw, CHUNK * ratio * sizeof(int16_t), &got_raw, pdMS_TO_TICKS(200)) != ESP_OK ||
        !got_raw) {
      continue;
    }
    const size_t n_raw = got_raw / sizeof(int16_t) / ratio * ratio;
    if (!mic_muted && ratio == RATIO) listen_for_claims(raw, n_raw);
    decimate(raw, n_raw, buf);
    const size_t n = n_raw / ratio;
    const size_t got = n * sizeof(int16_t);
    const bool speech = !mic_muted && is_speech(buf, n);
    if (!listen_on && !mic_muted) wakeword::feed(buf, n);  // between sessions, the wake word listens
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

bool begin(bool ultrasonic) {
  ratio = ultrasonic ? RATIO : 1;
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
  fir_init();
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
  LOGI(TAG, "ES8311 ready, %d Hz (%d Hz to and from the codec)", SAMPLE_RATE, SAMPLE_RATE * ratio);
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

bool playing() { return ok && (sound_playing || uxQueueMessagesWaiting(queue)); }

uint32_t soundMs(Sound sound) {
  const char* slot = slot_for(sound);
  if (fs_ok && slot && LittleFS.exists(clip_path(slot))) {
    File f = LittleFS.open(clip_path(slot), "r");
    const uint32_t ms = f ? f.size() * 1000 / (SAMPLE_RATE * 2) : 0;
    f.close();
    return ms;
  }
  if (sound == Sound::Claim) return claimMs(claim_level);
  uint32_t ms = 0;
  for (const Note* n = notes_for(sound); n->ms; n++) ms += n->ms;
  return ms;
}

bool soundsBegin() {
  fs_ok = LittleFS.begin(true, "/sounds", 4, "storage");
  if (!fs_ok) LOGE(TAG, "couldn't mount the storage partition: built-in sounds only");
  return fs_ok;
}

String soundSource(const char* slot) {
  if (!fs_ok) return "";
  File f = LittleFS.open(String("/") + slot + ".url", "r");
  if (!f) return "";
  String url = f.readString();
  f.close();
  return url;
}

bool storeSound(const char* slot, const String& url, const uint8_t* pcm, size_t bytes) {
  if (!fs_ok) return false;
  const String tmp = String("/") + slot + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const bool written = f.write(pcm, bytes) == bytes;
  f.close();
  if (!written) {
    LittleFS.remove(tmp);
    LOGW(TAG, "out of flash for the %s sound", slot);
    return false;
  }
  LittleFS.remove(clip_path(slot));
  LittleFS.rename(tmp, clip_path(slot));  // never a half-written clip
  File u = LittleFS.open(String("/") + slot + ".url", "w");
  if (u) {
    u.print(url);
    u.close();
  }
  return true;
}

uint8_t* readSound(const char* slot, size_t* size) {
  *size = 0;
  if (!fs_ok) return nullptr;
  File f = LittleFS.open(clip_path(slot), "r");
  if (!f) return nullptr;
  auto* data = static_cast<uint8_t*>(heap_caps_malloc(f.size() ? f.size() : 1, MALLOC_CAP_SPIRAM));
  if (data) *size = f.read(data, f.size());
  f.close();
  return data;
}

void removeSound(const char* slot) {
  if (!fs_ok) return;
  LittleFS.remove(clip_path(slot));
  LittleFS.remove(String("/") + slot + ".url");
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

bool ultrasonic() { return ratio == RATIO; }

void claim(int level) {
  if (!ok || ratio != RATIO) return;
  claim_level = constrain(level, 0, CLAIM_LEVELS - 1);
  const Sound s = Sound::Claim;
  xQueueSend(queue, &s, 0);
}

uint32_t claimMs(int level) { return claim_start_ms(2, constrain(level, 0, CLAIM_LEVELS - 1)) + CLAIM_TONE_MS; }

HeardClaim lastClaim() {
  portENTER_CRITICAL(&heard_lock);
  const HeardClaim c = heard;
  portEXIT_CRITICAL(&heard_lock);
  return c;
}

int dbfs(float level) { return static_cast<int>(lroundf(20.0f * log10f(max(level, 1.0f) / 32767.0f))); }

MicStats micStats() {
  MicStats s;
  s.average_db = dbfs(stats_count ? stats_sum / stats_count : 0);
  s.loudest_db = dbfs(stats_max);
  s.noise_floor = static_cast<int>(noise_floor);
  stats_sum = stats_max = 0;
  stats_count = 0;
  return s;
}

int recentLevelDb() {
  uint16_t loudest = 1;
  for (uint16_t l : levels) loudest = max(loudest, l);
  return static_cast<int>(lroundf(20.0f * log10f(loudest / 32767.0f)));
}

}  // namespace audio
