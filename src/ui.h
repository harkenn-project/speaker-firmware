// Screens. Every function here is safe to call from any task: LVGL access is serialized by an internal lock.
#pragma once
#include <Arduino.h>

#include <array>
#include <memory>
#include <vector>

namespace ui {

// Character states, in the order used by CharacterSet.
constexpr const char* STATES[] = {"idle", "listening", "thinking", "speaking", "sleeping", "error", "muted",
                                  "ringing"};
constexpr size_t STATE_COUNT = 8;
int stateIndex(const char* state);  // -1 if unknown

struct Blob {  // a downloaded file (KANI animation) held in PSRAM
  uint8_t* data = nullptr;
  size_t size = 0;
  ~Blob();
};
using CharacterSet = std::array<std::shared_ptr<Blob>, STATE_COUNT>;  // null entries fall back to idle

struct Theme {
  uint32_t background = 0x0b1020, surface = 0x1a2240, primary = 0x7aa2ff, accent = 0xff7ab6, text = 0xe8ecff,
           muted = 0x8a93b8;
};

struct Widget {
  String type;  // ring, character, clock, date, status
  int x = 0, y = 0, w = 0, h = 0, lines = 1, thickness = 4;
  String font = "md";
};

void begin();
void loop();  // run LVGL; call from the Arduino loop

// Full-screen messages shown before the speaker is online.
void showMessage(const char* title, const char* detail, bool spinner = true);
// The hotspot is up: join `apName` (with the speaker password when `secured`) and open 192.168.4.1.
void showWifiSetup(const char* title, const char* apName, bool secured);
void showAdoptionCode(const char* code, const char* host);
// A progress bar (0-100); calling it again with the same title only updates the bar and the detail text.
void showProgress(const char* title, const char* detail, int percent);

// The configured home screen.
void showLayout(const Theme& theme, const std::vector<Widget>& widgets, bool clock24h);
void setCharacter(const CharacterSet& set);
void setState(const char* state);
const char* state();
void setStatusText(const char* text);  // empty = default text for the state
void identify();                       // blink the screen for a few seconds

// Mic muted: while idle or sleeping, the character shows its muted animation and the ring turns orange.
void setMuted(bool muted);
// While the screen sleeps with the "clock" display: only a big clock, over everything else.
void showSleepClock(bool on);
// The status row at the top of every screen: remote (through the relay), an alarm set, an unread notification (it
// pulses when it arrives; a tap on it calls the onNotificationTap callback) and the next timer's time left.
void setStatusIcons(bool remote, bool alarm, bool notification, const char* timer_text);
void setRemote(bool on);
void onNotificationTap(void (*fn)());

// A timer's countdown over everything: a draining ring, the time left and its name, for `seconds` or until a tap.
void showCountdown(const char* name, time_t due, uint32_t total_s, uint32_t seconds);
void hideCountdown();
// Something ringing (a timer, an alarm): a shaking bell in a pulsing halo, what it is, Stop and (with on_snooze)
// Snooze. A tap anywhere else stops it too. The callbacks run on the UI task with the UI locked.
void showRinging(const char* title, const char* detail, const char* snooze_text, void (*on_stop)(),
                 void (*on_snooze)());
void hideRinging();
bool ringingShown();
// A notification card that drops in with a bouncing envelope and rises away after `seconds`; a tap on it calls
// on_seen (UI task, UI locked).
void showNotification(const char* title, const char* text, uint32_t seconds, void (*on_seen)());

// "Update available vX" with Install / Dismiss on top of the home screen, until hidden. The buttons call the
// callbacks (on the UI task, UI locked) and hide the prompt.
// System notices (the speaker telling you something about itself, e.g. the SD card): a card that pops up in the
// middle, with an icon in a tinted badge, a title and a line or two. Not mail (that's showNotification). With no
// buttons it goes after `seconds`; with one or two it stays until one is tapped. Text is plain ASCII: the fonts have
// no other characters. A new notice replaces the one showing.
constexpr const char* ICON_SD_CARD = "ï";  // LV_SYMBOL_SD_CARD
enum class NoticeKind : uint8_t { Info, Good, Warning };
struct NoticeButton {
  const char* label = nullptr;
  void (*on_tap)() = nullptr;
};
void showNotice(const char* icon, const char* title, const char* text, NoticeKind kind, uint32_t seconds,
                NoticeButton primary = {}, NoticeButton secondary = {});
void hideNotice();
// The media player on the screen. With an SD card in, a button on the home screen opens Media: Music (albums, then
// tracks), Soundscapes and Meditations, and the player (play/pause, previous/next, stop, repeat, a sleep timer, how far
// through around the edge). While something plays, the button is a "now playing" pill that opens the player. The
// handlers run on the UI task with the UI locked: they should only queue things (player::...).
struct MediaLibrary {
  std::vector<String> albums, soundscapes, meditations;
};
struct MediaNow {
  bool active = false, playing = false;
  String kind = "music", album, title, repeat = "off";
  int track = 0, tracks = 0;
  uint32_t elapsed_s = 0, duration_s = 0, sleep_s = 0;
};
struct MediaHandlers {
  void (*play)(const char* kind, const char* name, int track) = nullptr;
  void (*toggle)() = nullptr;
  void (*stop)() = nullptr;
  void (*next)() = nullptr;
  void (*previous)() = nullptr;
  void (*repeat)() = nullptr;  // off -> all -> one -> off
  void (*sleep)() = nullptr;   // off -> 15 -> 30 -> 60 minutes -> off
  std::vector<String> (*tracks)(const char* album) = nullptr;
};
void setMediaLibrary(bool card, const MediaLibrary& library);  // no card: the button goes
void setMediaNow(const MediaNow& now);
void onMedia(const MediaHandlers& handlers);
void hideMedia();  // closes the media screens (the speaker's about to listen or talk)
bool mediaShown();

void showUpdateOffer(const char* version, void (*on_install)(), void (*on_dismiss)());
void hideUpdateOffer();

// The assistant's screen, over the home screen: a picture (KANI, may be animated) with tappable areas. A tap on an
// area calls on_tap(label); a tap anywhere else calls on_tap("") when tap_dismisses (the app hides it). Gone with
// hideScreen() or a new layout. on_tap runs on the UI task with the UI locked: keep it short.
struct TapRegion {
  String label;
  int x = 0, y = 0, w = 0, h = 0;
};
void showScreen(const std::shared_ptr<Blob>& image, const std::vector<TapRegion>& regions, bool tap_dismisses);
void hideScreen();
bool screenShown();
void onScreenTap(void (*on_tap)(const char* label));

// Touch on the home screen. A tap in the middle calls `on_tap`; dragging around the edge turns the volume up
// (clockwise) or down, calling `on_volume` with the change in percent, which returns the new volume to display.
// Both run on the UI task with the UI locked: keep them short.
void onTouch(void (*on_tap)(), int (*on_volume)(int delta));

}  // namespace ui
