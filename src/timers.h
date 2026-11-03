// Timers and alarms, kept on the speaker so they ring on time even when Harkenn can't be reached, and survive
// restarts (saved in NVS). A timer goes off once, a set time after it was started; an alarm at a time of day, once or
// on chosen days of the week, in the speaker's time zone. Both need the clock (set from Harkenn or NTP).
//
// This keeps the list and works out what's due; app.cpp rings, and Harkenn sets, lists and cancels them
// (docs/PROTOCOL.md, "Timers and alarms"). Every function is safe to call from any task.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

namespace timers {

constexpr size_t MAX = 10;

struct Item {
  uint32_t id = 0;
  bool alarm = false;
  String label;
  time_t due = 0;          // when it next goes off (Unix time)
  uint32_t duration = 0;   // a timer's length, in seconds
  int8_t hour = -1, minute = -1;  // an alarm's time of day
  uint8_t days = 0;        // an alarm's days: bit 0 Sunday ... bit 6 Saturday; 0 = once
};

void begin();  // load the saved ones
std::vector<Item> list();  // soonest first
// Adds one; returns its id, or 0 if there are already MAX or the clock isn't set.
uint32_t addTimer(const String& label, uint32_t seconds);
// `date` "YYYY-MM-DD" for a one-off on that day; else the next time that time comes round (on `days`, if any).
uint32_t addAlarm(const String& label, int hour, int minute, uint8_t days, const String& date = "");
bool cancel(uint32_t id);
// Cancels by label (case-insensitive), or "all", "timers", "alarms". Returns how many.
size_t cancelMatching(const String& what);
// The soonest that's due (its time has come), taken off the list (a repeating alarm moves to its next time).
bool takeDue(Item* out);
// Rings again in `minutes`: a one-off copy of `item`.
uint32_t snooze(const Item& item, uint32_t minutes);
// Goes up on every change, so the app knows to tell Harkenn.
uint32_t version();
// For Harkenn and the web page: [{id, kind, label, due, remaining_s, duration, time, days}].
void toJson(JsonArray out);
// A short description of when an alarm goes off: "7:30 weekdays".
String describe(const Item& item);

}  // namespace timers
