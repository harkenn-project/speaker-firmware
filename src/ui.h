// Screens. Every function here is safe to call from any task: LVGL access is serialized by an internal lock.
#pragma once
#include <Arduino.h>

#include <array>
#include <memory>
#include <vector>

namespace ui {

// Character states, in the order used by CharacterSet.
constexpr const char* STATES[] = {"idle", "listening", "thinking", "speaking", "sleeping", "error"};
constexpr size_t STATE_COUNT = 6;
int stateIndex(const char* state);  // -1 if unknown

struct Blob {  // a GIF file held in PSRAM
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

// The configured home screen.
void showLayout(const Theme& theme, const std::vector<Widget>& widgets, bool clock24h);
void setCharacter(const CharacterSet& set);
void setState(const char* state);
const char* state();
void setStatusText(const char* text);  // empty = default text for the state
void identify();                       // blink the screen for a few seconds

}  // namespace ui
