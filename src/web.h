// The speaker's own web page, for break-glass access: on its network at http://<its IP>, and on its hotspot at
// http://192.168.4.1 when it can't join any saved Wi-Fi. Status, Wi-Fi networks (with static IP), the server
// address, log level, logs, firmware upload, restart and factory reset.
//
// It asks for the Harkenn admin password or the speaker's own password; only their hashes are kept (store.h). After
// 3 wrong passwords it waits 30 s, doubling every other try; after 10 it locks until Harkenn unlocks it. A speaker
// that isn't adopted yet has no passwords, so its page is open: that is the first-time Wi-Fi setup.
//
// Settings changed here are kept as local changes (store.h) and sent to Harkenn, where they win over its config.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace web {

struct Hooks {
  void (*status)(JsonObject out);   // live status, for the page
  void (*set_volume)(int volume);   // 0-100
  void (*set_brightness)(int percent);
  void (*wifi_changed)();           // the saved networks changed: use them now
  void (*server_changed)();         // the server address changed: reconnect
  void (*changes_recorded)();       // local changes to send to Harkenn
  bool (*check_update)();           // ask Harkenn for firmware; false if not connected
  bool (*install_offered)();        // install the firmware Harkenn offered; false if none
  void (*restart)();
};

// Starts serving (in its own task) on every interface.
void begin(const Hooks& hooks);
// The hotspot is up (or down): answer every DNS name with its address, so phones open the page by themselves.
// `physical`: opened with the button, by someone holding the speaker.
void setHotspot(bool on, bool physical);
bool hotspotOn();
// Someone logged in has used the page in the last `ms` (so the hotspot shouldn't be disturbed).
bool inUse(uint32_t ms);
// Harkenn's unlock counter changed: forget the wrong passwords.
void unlock();
// For the status sent to Harkenn.
uint8_t failures();
bool locked();

}  // namespace web
