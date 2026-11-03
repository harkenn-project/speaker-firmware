#include "timers.h"

#include <Preferences.h>

#include <algorithm>

#include "logbuf.h"

namespace timers {
namespace {

constexpr const char* TAG = "timers";
constexpr const char* DAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

Preferences prefs;
SemaphoreHandle_t mutex;
std::vector<Item> items;
uint32_t next_id = 1;
volatile uint32_t changes = 0;

struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

bool clock_set() { return time(nullptr) > 1700000000; }

// The next time an alarm's time of day comes round, strictly after `after`, on one of its days if it has any.
time_t next_alarm(int hour, int minute, uint8_t days, time_t after) {
  for (int add = 0; add < 8; add++) {
    struct tm t;
    localtime_r(&after, &t);
    t.tm_mday += add;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;  // let mktime work out daylight saving for that day
    const time_t when = mktime(&t);
    struct tm check;
    localtime_r(&when, &check);
    if (when > after && (!days || (days & (1 << check.tm_wday)))) return when;
  }
  return 0;
}

void sort() {
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.due < b.due; });
}

// Saved as one JSON document; the list is short.
void save() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const Item& i : items) {
    JsonObject o = arr.add<JsonObject>();
    o["i"] = i.id;
    o["a"] = i.alarm;
    o["l"] = i.label;
    o["d"] = static_cast<int64_t>(i.due);
    if (i.duration) o["s"] = i.duration;
    if (i.alarm) {
      o["h"] = i.hour;
      o["m"] = i.minute;
      o["w"] = i.days;
    }
  }
  String out;
  serializeJson(doc, out);
  prefs.putString("list", out);
  prefs.putUInt("next", next_id);
  changes = changes + 1;
}

}  // namespace

void begin() {
  mutex = xSemaphoreCreateMutex();
  prefs.begin("timers", false);
  Lock l;
  next_id = prefs.isKey("next") ? prefs.getUInt("next", 1) : 1;
  JsonDocument doc;
  if (prefs.isKey("list") && !deserializeJson(doc, prefs.getString("list", "[]"))) {
    for (JsonObject o : doc.as<JsonArray>()) {
      Item i;
      i.id = o["i"] | 0u;
      i.alarm = o["a"] | false;
      i.label = o["l"] | "";
      i.due = static_cast<time_t>(o["d"] | static_cast<int64_t>(0));
      i.duration = o["s"] | 0u;
      i.hour = o["h"] | -1;
      i.minute = o["m"] | -1;
      i.days = o["w"] | 0;
      if (i.id && i.due) items.push_back(i);
    }
  }
  sort();
  if (items.size()) LOGI(TAG, "%u timers and alarms kept", static_cast<unsigned>(items.size()));
}

std::vector<Item> list() {
  Lock l;
  return items;
}

uint32_t addTimer(const String& label, uint32_t seconds) {
  if (!clock_set() || !seconds) return 0;
  Lock l;
  if (items.size() >= MAX) return 0;
  Item i;
  i.id = next_id++;
  i.label = label.substring(0, 40);
  i.duration = seconds;
  i.due = time(nullptr) + seconds;
  items.push_back(i);
  sort();
  save();
  LOGI(TAG, "timer %u \"%s\": %u s", i.id, i.label.c_str(), static_cast<unsigned>(seconds));
  return i.id;
}

uint32_t addAlarm(const String& label, int hour, int minute, uint8_t days, const String& date) {
  if (!clock_set() || hour < 0 || hour > 23 || minute < 0 || minute > 59) return 0;
  time_t due = 0;
  if (date.length() == 10) {  // YYYY-MM-DD, once
    struct tm t = {};
    t.tm_year = date.substring(0, 4).toInt() - 1900;
    t.tm_mon = date.substring(5, 7).toInt() - 1;
    t.tm_mday = date.substring(8, 10).toInt();
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
    due = mktime(&t);
    days = 0;
    if (due <= time(nullptr)) return 0;  // that's in the past
  } else {
    due = next_alarm(hour, minute, days & 0x7f, time(nullptr));
  }
  if (!due) return 0;
  Lock l;
  if (items.size() >= MAX) return 0;
  Item i;
  i.id = next_id++;
  i.alarm = true;
  i.label = label.substring(0, 40);
  i.hour = hour;
  i.minute = minute;
  i.days = days & 0x7f;
  i.due = due;
  items.push_back(i);
  sort();
  save();
  LOGI(TAG, "alarm %u \"%s\": %s", i.id, i.label.c_str(), describe(i).c_str());
  return i.id;
}

bool cancel(uint32_t id) {
  Lock l;
  const auto it = std::find_if(items.begin(), items.end(), [id](const Item& i) { return i.id == id; });
  if (it == items.end()) return false;
  LOGI(TAG, "%s %u cancelled", it->alarm ? "alarm" : "timer", id);
  items.erase(it);
  save();
  return true;
}

size_t cancelMatching(const String& what) {
  String w = what;
  w.trim();
  w.toLowerCase();
  Lock l;
  const size_t before = items.size();
  items.erase(std::remove_if(items.begin(), items.end(), [&w](const Item& i) {
                if (w == "all") return true;
                if (w == "timers") return !i.alarm;
                if (w == "alarms") return i.alarm;
                String label = i.label;
                label.toLowerCase();
                return label == w || String(i.id) == w;
              }),
              items.end());
  const size_t gone = before - items.size();
  if (gone) {
    save();
    LOGI(TAG, "%u cancelled (%s)", static_cast<unsigned>(gone), what.c_str());
  }
  return gone;
}

bool takeDue(Item* out) {
  if (!clock_set()) return false;
  const time_t now = time(nullptr);
  Lock l;
  if (items.empty() || items.front().due > now) return false;
  *out = items.front();
  items.erase(items.begin());
  if (out->alarm && out->days) {  // a repeating alarm: on to its next day
    Item next = *out;
    next.due = next_alarm(next.hour, next.minute, next.days, now);
    if (next.due) items.push_back(next);
  }
  sort();
  save();
  return true;
}

uint32_t snooze(const Item& item, uint32_t minutes) {
  if (!clock_set()) return 0;
  Lock l;
  if (items.size() >= MAX) return 0;
  Item i = item;
  i.id = next_id++;
  i.days = 0;  // a one-off
  i.due = time(nullptr) + minutes * 60;
  if (!i.alarm) i.duration = minutes * 60;
  items.push_back(i);
  sort();
  save();
  return i.id;
}

uint32_t version() { return changes; }

String describe(const Item& i) {
  char t[8];
  snprintf(t, sizeof(t), "%d:%02d", i.hour, i.minute);
  String out = t;
  if (i.days == 0x7f) out += " daily";
  else if (i.days == 0x3e) out += " weekdays";
  else if (i.days == 0x41) out += " weekends";
  else if (i.days) {
    for (int d = 0; d < 7; d++) {
      if (i.days & (1 << d)) out += String(" ") + DAY_NAMES[d];
    }
  }
  return out;
}

void toJson(JsonArray out) {
  const time_t now = time(nullptr);
  for (const Item& i : list()) {
    JsonObject o = out.add<JsonObject>();
    o["id"] = i.id;
    o["kind"] = i.alarm ? "alarm" : "timer";
    o["label"] = i.label;
    o["due"] = static_cast<int64_t>(i.due);
    o["remaining_s"] = static_cast<int64_t>(max<time_t>(0, i.due - now));
    if (i.alarm) {
      char t[6];
      snprintf(t, sizeof(t), "%02d:%02d", i.hour, i.minute);
      o["time"] = t;
      o["days"] = i.days;
    } else {
      o["duration"] = i.duration;
    }
  }
}

}  // namespace timers
