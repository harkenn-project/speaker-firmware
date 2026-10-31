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

}  // namespace logbuf
