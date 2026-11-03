#include "logbuf.h"

#include <Arduino.h>
#include <esp_log.h>
#include <time.h>

namespace logbuf {
namespace {

struct Line {
  uint32_t ts;
  char level;
  char tag[24];
  char message[160];
};

QueueHandle_t queue;
vprintf_like_t idf_vprintf;
volatile char upload_level = 'I';

// The recent lines for the web page: a ring in PSRAM.
constexpr size_t KEPT = 300;
struct Kept {
  uint32_t seq, up_ms;
  Line line;
};
Kept* kept = nullptr;
uint32_t kept_seq = 0;  // of the newest line
portMUX_TYPE kept_lock = portMUX_INITIALIZER_UNLOCKED;

int rank(char level) {
  switch (level) {
    case 'E': return 0;
    case 'W': return 1;
    case 'I': return 2;
    case 'D': return 3;
    default: return 4;
  }
}

void enqueue(char level, const char* tag, const char* message) {
  if (!queue || rank(level) > rank(upload_level)) return;
  Line line{};
  const time_t now = time(nullptr);
  line.ts = now > 1700000000 ? static_cast<uint32_t>(now) : 0;
  line.level = level;
  strlcpy(line.tag, tag, sizeof(line.tag));
  strlcpy(line.message, message, sizeof(line.message));
  xQueueSend(queue, &line, 0);  // drop when full rather than block
  if (kept) {
    const uint32_t up = millis();
    portENTER_CRITICAL(&kept_lock);
    Kept& k = kept[++kept_seq % KEPT];
    k.seq = kept_seq;
    k.up_ms = up;
    k.line = line;
    portEXIT_CRITICAL(&kept_lock);
  }
}

// ESP-IDF lines look like "I (1234) wifi: message\n", possibly wrapped in ANSI colour codes.
int idf_hook(const char* fmt, va_list args) {
  char buf[200];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);

  const char* p = buf;
  if (*p == '\033') {
    const char* m = strchr(p, 'm');
    p = m ? m + 1 : p;
  }
  if (strchr("EWIDV", p[0]) && p[1] == ' ' && p[2] == '(') {
    const char* tag = strstr(p, ") ");
    const char* colon = tag ? strstr(tag + 2, ": ") : nullptr;
    if (colon) {
      char tagbuf[24];
      strlcpy(tagbuf, tag + 2, min(sizeof(tagbuf), static_cast<size_t>(colon - tag - 1)));
      String msg(colon + 2);
      msg.trim();
      const int esc = msg.indexOf('\033');
      if (esc >= 0) msg.remove(esc);
      enqueue(p[0], tagbuf, msg.c_str());
    }
  }
  return idf_vprintf(fmt, args);
}

}  // namespace

void begin() {
  queue = xQueueCreate(64, sizeof(Line));
  kept = static_cast<Kept*>(heap_caps_calloc(KEPT, sizeof(Kept), MALLOC_CAP_SPIRAM));
  idf_vprintf = esp_log_set_vprintf(idf_hook);
}

void log(char level, const char* tag, const char* fmt, ...) {
  char message[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  Serial.printf("[%c] %s: %s\n", level, tag, message);
  enqueue(level, tag, message);
}

size_t drain(JsonArray lines, size_t max) {
  size_t n = 0;
  Line line;
  while (n < max && queue && xQueueReceive(queue, &line, 0) == pdTRUE) {
    JsonObject o = lines.add<JsonObject>();
    o["ts"] = line.ts;
    o["level"] = String(line.level);
    o["tag"] = line.tag;
    o["message"] = line.message;
    n++;
  }
  return n;
}

void setUploadLevel(char level) { upload_level = level; }

char uploadLevel() { return upload_level; }

uint32_t recent(JsonArray lines, uint32_t after, size_t max) {
  if (!kept) return 0;
  portENTER_CRITICAL(&kept_lock);
  const uint32_t newest = kept_seq;
  portEXIT_CRITICAL(&kept_lock);
  uint32_t from = after + 1;
  if (from > newest) return newest;  // nothing new (or the speaker restarted: the page starts over)
  if (newest >= KEPT && from <= newest - KEPT) from = newest - KEPT + 1;  // older ones are overwritten
  if (newest - from + 1 > max) from = newest - max + 1;
  for (uint32_t seq = from; seq <= newest && seq; seq++) {
    Kept k;
    portENTER_CRITICAL(&kept_lock);
    k = kept[seq % KEPT];
    portEXIT_CRITICAL(&kept_lock);
    if (k.seq != seq) continue;  // overwritten meanwhile
    JsonObject o = lines.add<JsonObject>();
    o["seq"] = k.seq;
    o["ts"] = k.line.ts;
    o["up_ms"] = k.up_ms;
    o["level"] = String(k.line.level);
    o["tag"] = k.line.tag;
    o["message"] = k.line.message;
  }
  return newest;
}

}  // namespace logbuf
