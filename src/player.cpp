#include "player.h"

#include <FS.h>
#include <SD_MMC.h>
#include <esp_random.h>
#include <math.h>

#include <algorithm>
#include <new>
#include <vector>

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

#include "audio.h"
#include "card.h"
#include "logbuf.h"
#include "resample.h"

namespace player {
namespace {

constexpr const char* TAG = "player";
constexpr uint32_t SLEEP_FADE_MS = 8000;  // the sleep timer fades out over this long
constexpr uint32_t STOP_FADE_MS = 60;     // stop, next and previous: just long enough not to click
constexpr size_t READ_BYTES = 16 * 1024;  // file reads (PSRAM)
constexpr size_t OUT_SAMPLES = 1152;      // resampled output per push

enum class State : uint8_t { Stopped, Playing, Paused };

// Commands from other tasks, run by the player task.
enum class Op : uint8_t { Play, Pause, Resume, Stop, Next, Previous, Repeat, Sleep };
struct Command {
  Op op;
  Kind kind;
  int value;  // track, repeat mode or minutes
  bool shuffle;
  char album[96];
};
QueueHandle_t commands = nullptr;

// What's playing: shared with toJson() under the lock.
SemaphoreHandle_t mutex = nullptr;  // a mutex, not a spinlock: the Strings allocate
struct Shared {
  State state = State::Stopped;
  Kind kind = Kind::Music;
  Repeat repeat = Repeat::All;
  bool shuffle = false;
  String album, title, error, path;
  int track = 0, tracks = 0;
  uint32_t sleep_at = 0;  // millis() when the sleep timer goes off; 0 = none
} shared;
volatile uint32_t changes = 0;

// The player task's own state.
std::vector<String> paths;   // the album's files, in play order
std::vector<int> order;      // indexes into paths (shuffled or not)
int position = 0;            // into order

String lower(String s) {
  s.toLowerCase();
  return s;
}

bool is_audio(const String& name) {
  const String l = lower(name);
  return l.endsWith(".mp3") || l.endsWith(".wav");
}

// "03 Clair de lune.mp3" -> "Clair de lune": the name to show and say.
String title_of(const String& path) {
  String name = path.substring(path.lastIndexOf('/') + 1);
  const int dot = name.lastIndexOf('.');
  if (dot > 0) name = name.substring(0, dot);
  int i = 0;
  while (i < static_cast<int>(name.length()) && isdigit(name[i])) i++;
  if (i > 0 && i < static_cast<int>(name.length()) && (name[i] == ' ' || name[i] == '-' || name[i] == '.')) {
    while (i < static_cast<int>(name.length()) && (name[i] == ' ' || name[i] == '-' || name[i] == '.')) i++;
    name = name.substring(i);
  }
  return name;
}

// The files to play: an album folder's tracks, or the one soundscape or meditation with that name.
std::vector<String> find(Kind kind, const String& album) {
  std::vector<String> found;
  const char* root = kind == Kind::Music ? "/Music" : kind == Kind::Soundscape ? "/Soundscapes" : "/Meditations";
  File dir = SD_MMC.open(root);
  if (!dir || !dir.isDirectory()) return found;
  const String want = lower(album);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String name = f.name();
    if (name.startsWith(".")) continue;
    if (kind == Kind::Music) {
      if (!f.isDirectory() || lower(name) != want) continue;
      File inner = SD_MMC.open(String(root) + "/" + name);
      for (File t = inner.openNextFile(); t; t = inner.openNextFile()) {
        const String tn = t.name();
        if (!t.isDirectory() && !tn.startsWith(".") && is_audio(tn)) found.push_back(String(root) + "/" + name + "/" + tn);
      }
      break;
    }
    if (!f.isDirectory() && is_audio(name) && lower(title_of(name)) == want) {
      found.push_back(String(root) + "/" + name);
      break;
    }
  }
  std::sort(found.begin(), found.end());
  return found;
}

// Decoders: mono 16-bit PCM out, at the file's own rate.

struct Source {
  File file;
  bool mp3 = false;
  int rate = 0, channels = 1;
  // WAV
  uint32_t data_start = 0, data_end = 0;
  // MP3
  mp3dec_t dec;
  uint8_t* in = nullptr;  // READ_BYTES, PSRAM
  size_t in_len = 0, in_pos = 0;
  int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int16_t frame_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t pcm_len = 0, pcm_pos = 0;  // mono samples decoded and not yet taken
  int16_t wav_buf[2 * OUT_SAMPLES];
  bool eof = false;

  void close() {
    if (file) file.close();
    eof = true;
  }

  // ID3v2 at the start of an MP3: skipped (it can hold cover art that looks like audio to a decoder).
  void skip_id3() {
    uint8_t h[10];
    if (file.read(h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
      const uint32_t size = (h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F);
      file.seek(10 + size + (h[5] & 0x10 ? 10 : 0));
    } else {
      file.seek(0);
    }
  }

  bool open_wav() {
    uint8_t h[12];
    if (file.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    int bits = 0;
    for (;;) {
      uint8_t c[8];
      if (file.read(c, 8) != 8) return false;
      const uint32_t size = c[4] | c[5] << 8 | c[6] << 16 | c[7] << 24;
      if (!memcmp(c, "fmt ", 4)) {
        uint8_t f[16];
        if (size < 16 || file.read(f, 16) != 16) return false;
        const int format = f[0] | f[1] << 8;
        channels = f[2] | f[3] << 8;
        rate = f[4] | f[5] << 8 | f[6] << 16 | f[7] << 24;
        bits = f[14] | f[15] << 8;
        if ((format != 1 && format != 0xFFFE) || bits != 16 || channels < 1 || channels > 2) return false;
        file.seek(file.position() + size - 16 + (size & 1));
      } else if (!memcmp(c, "data", 4)) {
        data_start = file.position();
        data_end = data_start + size;
        return rate > 0 && bits == 16;
      } else {
        file.seek(file.position() + size + (size & 1));
      }
    }
  }

  bool open(const String& path) {
    close();
    eof = false;
    pcm_len = pcm_pos = in_len = in_pos = 0;
    file = SD_MMC.open(path);
    if (!file) return false;
    mp3 = lower(path).endsWith(".mp3");
    if (!mp3) return open_wav();
    if (!in) in = static_cast<uint8_t*>(heap_caps_malloc(READ_BYTES, MALLOC_CAP_SPIRAM));
    if (!in) return false;
    mp3dec_init(&dec);
    skip_id3();
    rate = 0;
    return true;
  }

  // Back to the start, for a seamless loop (a WAV soundscape plays straight on).
  bool rewind() {
    if (!file) return false;
    eof = false;
    pcm_len = pcm_pos = in_len = in_pos = 0;
    if (!mp3) return file.seek(data_start);
    mp3dec_init(&dec);
    file.seek(0);
    skip_id3();
    return true;
  }

  bool decode_mp3_frame() {
    for (int tries = 0; tries < 64; tries++) {
      if (in_len - in_pos < 2048 && !eof) {  // top up the input
        memmove(in, in + in_pos, in_len - in_pos);
        in_len -= in_pos;
        in_pos = 0;
        const int got = file.read(in + in_len, READ_BYTES - in_len);
        if (got <= 0) eof = true;
        else in_len += got;
      }
      if (in_pos >= in_len) return false;
      mp3dec_frame_info_t info;
      int16_t* frame = frame_buf;
      const int samples = mp3dec_decode_frame(&dec, in + in_pos, in_len - in_pos, frame, &info);
      in_pos += info.frame_bytes;
      if (!info.frame_bytes) {
        if (eof) return false;
        in_pos = in_len;  // nothing decodable in what's buffered: read on
        continue;
      }
      if (!samples) continue;  // skipped data (a tag, or the decoder finding its feet)
      rate = info.hz;
      for (int i = 0; i < samples; i++) {
        pcm[i] = info.channels == 2 ? static_cast<int16_t>((frame[2 * i] + frame[2 * i + 1]) / 2) : frame[i];
      }
      pcm_len = samples;
      pcm_pos = 0;
      return true;
    }
    return false;
  }

  // Up to `max` mono samples; 0 at the end of the file.
  size_t read(int16_t* out, size_t max) {
    if (mp3) {
      if (pcm_pos >= pcm_len && !decode_mp3_frame()) return 0;
      const size_t n = min(max, pcm_len - pcm_pos);
      memcpy(out, pcm + pcm_pos, n * sizeof(int16_t));
      pcm_pos += n;
      return n;
    }
    const uint32_t left = data_end > file.position() ? data_end - file.position() : 0;
    const size_t frame = 2 * channels;
    const size_t want = min<size_t>(max, left / frame);
    if (!want) return 0;
    int16_t* raw = wav_buf;
    const size_t n = min(want, 2 * OUT_SAMPLES * sizeof(int16_t) / frame);
    const size_t got = file.read(reinterpret_cast<uint8_t*>(raw), n * frame) / frame;
    for (size_t i = 0; i < got; i++) {
      out[i] = channels == 2 ? static_cast<int16_t>((raw[2 * i] + raw[2 * i + 1]) / 2) : raw[i];
    }
    return got;
  }
};

// The decoder and the buffers live in PSRAM (begin()): together they're ~20 KB, too much for internal RAM.
Source* src = nullptr;

Resampler resampler;
int16_t* pending = nullptr;  // MINIMP3_MAX_SAMPLES_PER_FRAME, PSRAM: decoded, not yet resampled
int16_t* out = nullptr;      // OUT_SAMPLES, PSRAM: resampled, for the music stream
size_t pending_len = 0, pending_pos = 0;
bool draining = false;      // the last track has been read: stop once what's buffered has played
float gain = 1.0f;          // the sleep timer's fade
uint32_t fade_started = 0;  // millis() when the sleep fade began; 0 = none

void changed() { changes++; }

void set_error(const String& e) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  shared.error = e;
  xSemaphoreGive(mutex);
  if (e.length()) LOGW(TAG, "%s", e.c_str());
  changed();
}

void set_state(State s) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  shared.state = s;
  if (s == State::Stopped) shared.sleep_at = 0;
  xSemaphoreGive(mutex);
  audio::holdMusic(audio::HOLD_PAUSED, s != State::Playing);
  changed();
}

// Fades out what's playing (briefly) and drops it, ready for something else.
void cut() {
  if (audio::musicPlaying()) {
    audio::holdMusic(audio::HOLD_PAUSED, true);
    delay(STOP_FADE_MS);
  }
  audio::clearMusic();
  pending_len = pending_pos = 0;
}

bool open_track(int at) {
  if (at < 0 || at >= static_cast<int>(order.size())) return false;
  position = at;
  const String& path = paths[order[at]];
  if (!src->open(path)) {
    set_error("couldn't play " + path);
    return false;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  shared.track = order[at];
  shared.title = title_of(path);
  shared.path = path;
  xSemaphoreGive(mutex);
  LOGI(TAG, "playing %s", path.c_str());
  changed();
  return true;
}

void stop_now() {
  draining = false;
  cut();
  src->close();
  fade_started = 0;
  gain = 1.0f;
  set_state(State::Stopped);
}

void start(const Command& c) {
  draining = false;
  cut();
  src->close();
  if (!card::present()) {
    set_error("no SD card");
    set_state(State::Stopped);
    return;
  }
  std::vector<String> found = find(c.kind, c.album);
  if (found.empty()) {
    set_error(String("not on the card: ") + c.album);
    set_state(State::Stopped);
    return;
  }
  paths = found;
  order.clear();
  for (size_t i = 0; i < paths.size(); i++) order.push_back(i);
  if (c.shuffle) {
    for (size_t i = order.size(); i > 1; i--) std::swap(order[i - 1], order[esp_random() % i]);
  }
  int first = 0;
  if (!c.shuffle) first = constrain(c.value, 0, static_cast<int>(order.size()) - 1);
  xSemaphoreTake(mutex, portMAX_DELAY);
  shared.kind = c.kind;
  shared.album = c.album;
  shared.tracks = paths.size();
  shared.shuffle = c.shuffle;
  shared.repeat = c.kind == Kind::Music ? Repeat::All : c.kind == Kind::Soundscape ? Repeat::One : Repeat::Off;
  shared.error = "";
  xSemaphoreGive(mutex);
  fade_started = 0;
  gain = 1.0f;
  if (open_track(first)) set_state(State::Playing);
  else set_state(State::Stopped);
}

// The end of a track: what comes next depends on the repeat mode.
void track_ended() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  const Repeat repeat = shared.repeat;
  xSemaphoreGive(mutex);
  if (repeat == Repeat::One) {
    src->rewind();  // straight on: the resampler keeps its state, so a WAV loop has no seam
    return;
  }
  if (position + 1 < static_cast<int>(order.size())) {
    open_track(position + 1);
  } else if (repeat == Repeat::All) {
    open_track(0);
  } else {
    src->close();
    draining = true;  // feed() stops once what's buffered has played
  }
}

void skip(int by) {
  if (order.empty()) return;
  const int n = order.size();
  int to = position + by;
  to = (to % n + n) % n;
  draining = false;
  cut();
  if (open_track(to)) set_state(State::Playing);
}

void run(const Command& c) {
  switch (c.op) {
    case Op::Play: start(c); break;
    case Op::Pause:
      if (shared.state == State::Playing) set_state(State::Paused);
      break;
    case Op::Resume:
      if (shared.state == State::Paused) set_state(State::Playing);
      break;
    case Op::Stop: stop_now(); break;
    case Op::Next: skip(1); break;
    case Op::Previous: skip(-1); break;
    case Op::Repeat:
      xSemaphoreTake(mutex, portMAX_DELAY);
      shared.repeat = static_cast<Repeat>(constrain(c.value, 0, 2));
      xSemaphoreGive(mutex);
      changed();
      break;
    case Op::Sleep:
      xSemaphoreTake(mutex, portMAX_DELAY);
      shared.sleep_at = c.value > 0 ? (millis() + c.value * 60000UL) | 1 : 0;
      xSemaphoreGive(mutex);
      fade_started = 0;
      gain = 1.0f;
      changed();
      break;
  }
}

// Decodes and pushes as much as fits. Returns false when there was nothing to do.
bool feed() {
  if (shared.state != State::Playing) return false;
  if (draining) {
    if (!audio::musicQueued() && !audio::musicPlaying()) {
      draining = false;
      set_state(State::Stopped);
      return true;
    }
    return false;
  }
  const size_t space = audio::musicSpace();
  if (space < OUT_SAMPLES) return false;
  if (pending_pos >= pending_len) {
    pending_len = src->read(pending, MINIMP3_MAX_SAMPLES_PER_FRAME);
    pending_pos = 0;
    if (!pending_len) {
      track_ended();
      return true;
    }
  }
  resampler.setup(src->rate ? src->rate : 44100, audio::hwRate());
  size_t made = 0;
  pending_pos += resampler.run(pending + pending_pos, pending_len - pending_pos, out, OUT_SAMPLES, &made, gain);
  if (made) audio::writeMusic(out, made);
  return true;
}

void sleep_timer() {
  uint32_t at;
  xSemaphoreTake(mutex, portMAX_DELAY);
  at = shared.sleep_at;
  xSemaphoreGive(mutex);
  if (!at || shared.state == State::Stopped) return;
  const int32_t left = static_cast<int32_t>(at - millis());
  if (left > static_cast<int32_t>(SLEEP_FADE_MS)) return;
  if (!fade_started) {
    fade_started = millis() | 1;
    LOGI(TAG, "sleep timer: fading out");
  }
  gain = max(0.0f, 1.0f - static_cast<float>(millis() - fade_started) / SLEEP_FADE_MS);
  if (left <= 0 && gain <= 0) stop_now();
}

void task(void*) {
  for (;;) {
    Command c;
    if (xQueueReceive(commands, &c, 0) == pdTRUE) run(c);
    sleep_timer();
    if (!feed()) {
      if (xQueueReceive(commands, &c, pdMS_TO_TICKS(20)) == pdTRUE) run(c);
    }
  }
}

void send(Op op, Kind kind = Kind::Music, int value = 0, const String& album = "", bool shuffle = false) {
  if (!commands) return;
  Command c{};
  c.op = op;
  c.kind = kind;
  c.value = value;
  c.shuffle = shuffle;
  strlcpy(c.album, album.c_str(), sizeof(c.album));
  xQueueSend(commands, &c, pdMS_TO_TICKS(100));
}

}  // namespace

void begin() {
  commands = xQueueCreate(8, sizeof(Command));
  mutex = xSemaphoreCreateMutex();
  void* mem = heap_caps_calloc(1, sizeof(Source), MALLOC_CAP_SPIRAM);
  pending = static_cast<int16_t*>(heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  out = static_cast<int16_t*>(heap_caps_malloc(OUT_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (!mem || !pending || !out) {
    LOGE(TAG, "out of PSRAM for the player");
    return;
  }
  src = new (mem) Source();
  // minimp3 keeps about 16 KB of scratch on the stack while it decodes a frame; PSRAM keeps that out of the scarce
  // internal RAM.
  if (xTaskCreatePinnedToCoreWithCaps(task, "player", 32768, nullptr, 4, nullptr, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
    LOGE(TAG, "couldn't start the player");
  }
}

void play(Kind kind, const String& album, int track, bool shuffle) { send(Op::Play, kind, track, album, shuffle); }
void pause() { send(Op::Pause); }
void resume() { send(Op::Resume); }
void stop() { send(Op::Stop); }
void next() { send(Op::Next); }
void previous() { send(Op::Previous); }
void setRepeat(Repeat repeat) { send(Op::Repeat, Kind::Music, static_cast<int>(repeat)); }
void sleepAfter(uint32_t minutes) { send(Op::Sleep, Kind::Music, minutes); }

bool active() { return shared.state != State::Stopped; }

String playingPath() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  const String p = shared.state != State::Stopped ? shared.path : String();
  xSemaphoreGive(mutex);
  return p;
}

uint32_t version() { return changes; }

void toJson(JsonObject out) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  const Shared s = shared;
  xSemaphoreGive(mutex);
  out["state"] = s.state == State::Playing ? "playing" : s.state == State::Paused ? "paused" : "stopped";
  out["kind"] = kindName(s.kind);
  out["album"] = s.album;
  out["title"] = s.title;
  out["track"] = s.track;
  out["tracks"] = s.tracks;
  out["repeat"] = s.repeat == Repeat::All ? "all" : s.repeat == Repeat::One ? "one" : "off";
  out["shuffle"] = s.shuffle;
  if (s.sleep_at) out["sleep_s"] = max<int32_t>(0, static_cast<int32_t>(s.sleep_at - millis()) / 1000);
  if (s.error.length()) out["error"] = s.error;
}

const char* kindName(Kind kind) {
  return kind == Kind::Music ? "music" : kind == Kind::Soundscape ? "soundscape" : "meditation";
}

bool parseKind(const String& name, Kind* out) {
  if (name == "music") *out = Kind::Music;
  else if (name == "soundscape") *out = Kind::Soundscape;
  else if (name == "meditation") *out = Kind::Meditation;
  else return false;
  return true;
}

bool parseRepeat(const String& name, Repeat* out) {
  if (name == "off" || name == "once") *out = Repeat::Off;
  else if (name == "all") *out = Repeat::All;
  else if (name == "one") *out = Repeat::One;
  else return false;
  return true;
}

}  // namespace player
