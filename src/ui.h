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
// Connected through the relay (away from home): a small icon at the top of every screen.
void setRemote(bool on);

// "Update available vX" with Install / Dismiss on top of the home screen, until hidden. The buttons call the
// callbacks (on the UI task, UI locked) and hide the prompt.
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
