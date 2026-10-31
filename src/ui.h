// Screens. Every function here is safe to call from any task: LVGL access is serialized by an internal lock.
#pragma once
#include <Arduino.h>

#include <array>
#include <memory>
#include <vector>

namespace ui {

// Character states, in the order used by CharacterSet.
constexpr const char* STATES[] = {"idle", "listening", "thinking", "speaking", "sleeping", "error", "muted"};
constexpr size_t STATE_COUNT = 7;
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
void showWifiSetup(const char* apName);
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

// "Update available vX" with Install / Dismiss on top of the home screen, until hidden. The buttons call the
// callbacks (on the UI task, UI locked) and hide the prompt.
void showUpdateOffer(const char* version, void (*on_install)(), void (*on_dismiss)());
void hideUpdateOffer();

// Touch on the home screen. A tap in the middle calls `on_tap`; dragging around the edge turns the volume up
// (clockwise) or down, calling `on_volume` with the change in percent, which returns the new volume to display.
// Both run on the UI task with the UI locked: keep them short.
void onTouch(void (*on_tap)(), int (*on_volume)(int delta));

}  // namespace ui
