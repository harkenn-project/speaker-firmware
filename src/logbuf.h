// Logging: prints to the USB console and queues lines for upload to Kotodama.
// ESP-IDF's own log output (Wi-Fi, TLS, ...) is captured too.
#pragma once
#include <ArduinoJson.h>

namespace logbuf {

void begin();
void log(char level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
// Moves up to `max` queued lines into `lines` as {ts, level, tag, message}. Returns how many.
size_t drain(JsonArray lines, size_t max);
// Lines below this level are not uploaded (they still print): 'E', 'W', 'I' or 'D'.
void setUploadLevel(char level);

}  // namespace logbuf

#define LOGE(tag, ...) logbuf::log('E', tag, __VA_ARGS__)
#define LOGW(tag, ...) logbuf::log('W', tag, __VA_ARGS__)
#define LOGI(tag, ...) logbuf::log('I', tag, __VA_ARGS__)
#define LOGD(tag, ...) logbuf::log('D', tag, __VA_ARGS__)
