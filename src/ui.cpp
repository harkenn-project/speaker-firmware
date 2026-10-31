#include "ui.h"

#include <lvgl.h>
#include <math.h>
#include <time.h>

#include "board.h"

namespace ui {
namespace {

SemaphoreHandle_t mutex;
struct Lock {
  Lock() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(mutex); }
};

Theme theme;
bool clock_24h = true;
String current_state = "idle";
String status_text;
CharacterSet character;

lv_obj_t* ring = nullptr;
lv_obj_t* anim_img = nullptr;  // the character widget
lv_obj_t* clock_label = nullptr;
lv_obj_t* date_label = nullptr;
lv_obj_t* status_label = nullptr;
lv_timer_t* clock_timer = nullptr;
lv_obj_t* flash = nullptr;
lv_timer_t* flash_timer = nullptr;
bool muted = false;

// Touch: a transparent layer on top of the home screen, and the volume arc it shows.
constexpr int EDGE_RADIUS = 80;             // touches starting farther than this from the centre adjust the volume
constexpr float DEGREES_PER_PERCENT = 2.7f;  // the whole 270-degree arc is 100%
constexpr uint32_t VOLUME_HIDE_MS = 1500;
lv_obj_t* touch_layer = nullptr;
lv_obj_t* volume_arc = nullptr;
lv_obj_t* volume_label = nullptr;
lv_timer_t* volume_timer = nullptr;
void (*tap_cb)() = nullptr;
int (*volume_cb)(int) = nullptr;

struct Gesture {
  bool edge = false, dragging = false, cancelled = false;
  lv_point_t start{};
  float last_angle = 0, accum = 0;
  uint32_t t0 = 0;
} gesture;

lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

// KANI animation player (format: kotodama/app/kani.py). Frames are decoded into one RGB565 frame buffer in PSRAM
// that an lv_image displays; frame 0 is complete, later frames only carry changed pixels.

uint16_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

struct Player {
  std::shared_ptr<Blob> blob;
  uint16_t w = 0, h = 0, frames = 0, index = 0;
  uint16_t* fb = nullptr;
  size_t fb_pixels = 0;
  lv_image_dsc_t dsc{};
  lv_timer_t* timer = nullptr;
} player;

bool kani_valid(const Blob& b, uint16_t& w, uint16_t& h, uint16_t& n) {
  if (b.size < 12 || memcmp(b.data, "KANI", 4) != 0 || b.data[4] != 1) return false;
  w = rd16(b.data + 6);
  h = rd16(b.data + 8);
  n = rd16(b.data + 10);
  return w && h && n && w <= board::SCREEN_SIZE && h <= board::SCREEN_SIZE && 12 + 10u * n <= b.size;
}

uint16_t kani_frame_ms(const Blob& b, uint16_t i) { return max<uint16_t>(20, rd16(b.data + 12 + 10 * i)); }

bool kani_decode(const Blob& b, uint16_t i, uint16_t* fb, size_t total) {
  const uint32_t off = rd32(b.data + 12 + 10 * i + 2), len = rd32(b.data + 12 + 10 * i + 6);
  if (off > b.size || len > b.size - off) return false;
  const uint8_t* p = b.data + off;
  const uint8_t* end = p + len;
  size_t px = 0;
  while (p < end) {
    const uint8_t op = *p++;
    uint32_t count = op & 0x3F;
    if (count == 0x3F) {
      if (end - p < 2) return false;
      count += rd16(p);
      p += 2;
    }
    count += 1;
    if (px + count > total) return false;
    switch (op >> 6) {
      case 0:  // skip
        if (i == 0) return false;
        break;
      case 1: {  // run
        if (end - p < 2) return false;
        const uint16_t c = rd16(p);
        p += 2;
        for (uint32_t k = 0; k < count; k++) fb[px + k] = c;
        break;
      }
      case 2:  // literal
        if (static_cast<size_t>(end - p) < 2 * count) return false;
        memcpy(fb + px, p, 2 * count);
        p += 2 * count;
        break;
      default:
        return false;
    }
    px += count;
  }
  return px == total;
}

void player_stop() {
  if (player.timer) lv_timer_delete(player.timer);
  player.timer = nullptr;
  player.blob.reset();
}

void player_tick(lv_timer_t* t) {
  if (!player.blob || !anim_img) return;
  player.index = (player.index + 1) % player.frames;
  if (!kani_decode(*player.blob, player.index, player.fb, player.fb_pixels)) {
    player_stop();
    return;
  }
  lv_image_cache_drop(&player.dsc);
  lv_obj_invalidate(anim_img);
  lv_timer_set_period(t, kani_frame_ms(*player.blob, player.index));
}

void player_play(const std::shared_ptr<Blob>& blob) {
  if (!anim_img || blob == player.blob) return;
  player_stop();
  uint16_t w, h, n;
  if (!blob || !kani_valid(*blob, w, h, n)) {
    lv_obj_add_flag(anim_img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const size_t pixels = static_cast<size_t>(w) * h;
  if (pixels != player.fb_pixels) {
    heap_caps_free(player.fb);
    player.fb = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM));
    player.fb_pixels = player.fb ? pixels : 0;
    if (!player.fb) return;
  }
  if (!kani_decode(*blob, 0, player.fb, pixels)) {
    lv_obj_add_flag(anim_img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  player.blob = blob;
  player.w = w;
  player.h = h;
  player.frames = n;
  player.index = 0;
  player.dsc = {};
  player.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  player.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  player.dsc.header.w = w;
  player.dsc.header.h = h;
  player.dsc.header.stride = w * 2;
  player.dsc.data = reinterpret_cast<const uint8_t*>(player.fb);
  player.dsc.data_size = pixels * 2;
  lv_image_cache_drop(&player.dsc);
  lv_image_set_src(anim_img, &player.dsc);
  lv_obj_remove_flag(anim_img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(anim_img);
  if (n > 1) player.timer = lv_timer_create(player_tick, kani_frame_ms(*blob, 0), nullptr);
}

const lv_font_t* font(const String& size) {
  if (size == "sm") return &lv_font_montserrat_14;
  if (size == "lg") return &lv_font_montserrat_28;
  if (size == "xl") return &lv_font_montserrat_48;
  return &lv_font_montserrat_18;
}

// Removes everything from the screen and paints the theme background.
lv_obj_t* reset_screen() {
  lv_obj_t* scr = lv_screen_active();
  player_stop();
  lv_obj_clean(scr);
  ring = anim_img = clock_label = date_label = status_label = flash = nullptr;
  touch_layer = volume_arc = volume_label = nullptr;
  if (flash_timer) {
    lv_timer_delete(flash_timer);
    flash_timer = nullptr;
  }
  if (volume_timer) {
    lv_timer_delete(volume_timer);
    volume_timer = nullptr;
  }
  lv_obj_set_style_bg_color(scr, color(theme.background), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

// A centred label in the box (x, y, w, h). Text never leaves the box: a one-line label is cut short with "...",
// a multi-line label wraps and is clipped after `lines` lines. The text is vertically centred in the box.
lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* f, uint32_t rgb, int x, int y, int w, int h,
                int lines = 1) {
  const int line_h = lv_font_get_line_height(f);
  lines = max(1, min(lines, h / line_h));
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, color(rgb), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, lines == 1 ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
  lv_obj_set_size(l, w, line_h * lines);
  lv_label_set_text(l, text);
  lv_obj_set_pos(l, x, y + max(0, (h - line_h * lines) / 2));
  return l;
}

// What the character shows: muted replaces idle and sleeping; activity (listening, speaking, ...) shows as usual.
String shown_state() {
  if (muted && (current_state == "idle" || current_state == "sleeping")) return "muted";
  return current_state;
}

uint32_t ring_color(const String& state) {
  if (state == "muted") return 0xff9640;
  if (state == "listening") return theme.primary;
  if (state == "thinking") return theme.muted;
  if (state == "speaking") return theme.accent;
  if (state == "error") return 0xff5d5d;
  if (state == "sleeping") return theme.background;
  return theme.surface;
}

const char* default_status(const String& state) {
  if (state == "listening") return "Listening...";
  if (state == "thinking") return "Thinking...";
  if (state == "error") return "Something went wrong";
  if (state == "muted") return "Mic off";
  return "";
}

void update_clock(lv_timer_t* = nullptr) {
  if (!clock_label && !date_label) return;
  const time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  const bool valid = now > 1700000000;
  char buf[32];
  if (clock_label) {
    if (!valid) strcpy(buf, "--:--");
    else if (clock_24h) strftime(buf, sizeof(buf), "%H:%M", &t);
    else snprintf(buf, sizeof(buf), "%d:%02d", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min);
    lv_label_set_text(clock_label, buf);
  }
  if (date_label) {
    if (valid) strftime(buf, sizeof(buf), "%a %e %b", &t);
    else buf[0] = 0;
    lv_label_set_text(date_label, buf);
  }
}

void apply_state() {
  const String state = shown_state();
  if (ring) lv_obj_set_style_border_color(ring, color(ring_color(state)), 0);
  if (status_label) {
    lv_label_set_text(status_label, status_text.length() ? status_text.c_str() : default_status(state));
  }
  if (anim_img) {
    int i = stateIndex(state.c_str());
    if (i < 0 || !character[i]) i = 0;
    player_play(character[i]);
  }
}

// Volume arc

void show_volume(int percent) {
  if (!volume_arc) return;
  lv_arc_set_value(volume_arc, percent);
  lv_label_set_text_fmt(volume_label, percent ? "%d%%" : "Silent", percent);
  lv_obj_remove_flag(volume_arc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(volume_label, LV_OBJ_FLAG_HIDDEN);
  lv_timer_reset(volume_timer);
  lv_timer_resume(volume_timer);
}

void hide_volume(lv_timer_t* t) {
  lv_obj_add_flag(volume_arc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(volume_label, LV_OBJ_FLAG_HIDDEN);
  lv_timer_pause(t);
}

void create_touch_layer(lv_obj_t* scr) {
  volume_arc = lv_arc_create(scr);
  lv_obj_set_size(volume_arc, board::SCREEN_SIZE - 8, board::SCREEN_SIZE - 8);
  lv_obj_set_pos(volume_arc, 4, 4);
  lv_arc_set_rotation(volume_arc, 135);  // gap at the bottom; 0% bottom-left, 100% bottom-right
  lv_arc_set_bg_angles(volume_arc, 0, 270);
  lv_arc_set_range(volume_arc, 0, 100);
  lv_obj_remove_flag(volume_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(volume_arc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(volume_arc, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(volume_arc, color(theme.surface), LV_PART_MAIN);
  lv_obj_set_style_arc_color(volume_arc, color(theme.primary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(volume_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(volume_arc, 0, LV_PART_KNOB);
  lv_obj_add_flag(volume_arc, LV_OBJ_FLAG_HIDDEN);

  volume_label = label(scr, "", &lv_font_montserrat_24, theme.text, 60, 196, 120, 30);
  lv_obj_add_flag(volume_label, LV_OBJ_FLAG_HIDDEN);
  volume_timer = lv_timer_create(hide_volume, VOLUME_HIDE_MS, nullptr);
  lv_timer_pause(volume_timer);

  touch_layer = lv_obj_create(scr);
  lv_obj_remove_style_all(touch_layer);
  lv_obj_set_size(touch_layer, board::SCREEN_SIZE, board::SCREEN_SIZE);
  lv_obj_set_pos(touch_layer, 0, 0);
  lv_obj_remove_flag(touch_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(
      touch_layer,
      [](lv_event_t* e) {
        const lv_event_code_t code = lv_event_get_code(e);
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        const float dx = p.x - board::SCREEN_SIZE / 2, dy = p.y - board::SCREEN_SIZE / 2;
        const float r = sqrtf(dx * dx + dy * dy);
        const float angle = atan2f(dy, dx) * 180.0f / PI;  // clockwise on screen
        Gesture& g = gesture;
        if (code == LV_EVENT_PRESSED) {
          g = Gesture{};
          g.edge = r >= EDGE_RADIUS;
          g.start = p;
          g.last_angle = angle;
          g.t0 = millis();
        } else if (code == LV_EVENT_PRESSING) {
          if (g.edge) {
            if (r < 40) return;  // the angle is meaningless near the centre
            float d = angle - g.last_angle;
            while (d > 180) d -= 360;
            while (d < -180) d += 360;
            g.last_angle = angle;
            g.accum += d;
            if (!g.dragging && fabsf(g.accum) > 8) g.dragging = true;
            const int steps = static_cast<int>(g.accum / DEGREES_PER_PERCENT);
            if (g.dragging && steps && volume_cb) {
              g.accum -= steps * DEGREES_PER_PERCENT;
              show_volume(volume_cb(steps));
            }
          } else if (abs(p.x - g.start.x) + abs(p.y - g.start.y) > 24) {
            g.cancelled = true;  // a swipe, not a tap
          }
        } else if (code == LV_EVENT_RELEASED) {
          if (!g.edge && !g.cancelled && millis() - g.t0 < 1000 && tap_cb) tap_cb();
        }
      },
      LV_EVENT_ALL, nullptr);
}

}  // namespace

Blob::~Blob() {
  if (data) heap_caps_free(data);
}

int stateIndex(const char* state) {
  for (size_t i = 0; i < STATE_COUNT; i++)
    if (strcmp(STATES[i], state) == 0) return i;
  return -1;
}

void begin() {
  mutex = xSemaphoreCreateRecursiveMutex();
  clock_timer = lv_timer_create(update_clock, 1000, nullptr);
}

void loop() {
  uint32_t wait;
  {
    Lock l;
    wait = lv_timer_handler();
  }
  delay(constrain(wait, 1, 10));
}

void showMessage(const char* title, const char* detail, bool spinner) {
  Lock l;
  lv_obj_t* scr = reset_screen();
  if (spinner) {
    lv_obj_t* s = lv_spinner_create(scr);
    lv_spinner_set_anim_params(s, 1200, 60);
    lv_obj_set_size(s, 56, 56);
    lv_obj_set_pos(s, 92, 44);
    lv_obj_set_style_arc_color(s, color(theme.surface), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s, color(theme.primary), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s, 6, LV_PART_INDICATOR);
  }
  const int top = spinner ? 108 : 72;
  label(scr, title, &lv_font_montserrat_18, theme.text, 26, top, 188, 44, 2);
  label(scr, detail, &lv_font_montserrat_14, theme.muted, 40, top + 46, 160, 54, 3);
}

void showWifiSetup(const char* apName) {
  Lock l;
  lv_obj_t* scr = reset_screen();
  label(scr, "Wi-Fi setup", &lv_font_montserrat_18, theme.text, 40, 40, 160, 24);
  label(scr, "Join the network", &lv_font_montserrat_14, theme.muted, 30, 76, 180, 18);
  label(scr, apName, &lv_font_montserrat_24, theme.primary, 14, 98, 212, 30);
  label(scr, "then open", &lv_font_montserrat_14, theme.muted, 40, 140, 160, 18);
  label(scr, "192.168.4.1", &lv_font_montserrat_18, theme.text, 40, 160, 160, 24);
}

void showAdoptionCode(const char* code, const char* host) {
  Lock l;
  lv_obj_t* scr = reset_screen();
  lv_obj_t* r = lv_obj_create(scr);
  lv_obj_set_size(r, 240, 240);
  lv_obj_set_pos(r, 0, 0);
  lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 4, 0);
  lv_obj_set_style_border_color(r, color(theme.accent), 0);
  label(scr, "Adoption code", &lv_font_montserrat_18, theme.text, 30, 48, 180, 24);
  lv_obj_t* c = label(scr, code, &lv_font_montserrat_48, theme.primary, 20, 82, 200, 64);
  lv_obj_set_style_text_letter_space(c, 8, 0);
  label(scr, "Enter it in Kotodama", &lv_font_montserrat_14, theme.muted, 30, 150, 180, 20);
  label(scr, host, &lv_font_montserrat_14, theme.muted, 20, 170, 200, 20);
}

void showLayout(const Theme& t, const std::vector<Widget>& widgets, bool clock24h) {
  Lock l;
  theme = t;
  clock_24h = clock24h;
  lv_obj_t* scr = reset_screen();
  const Widget* ring_widget = nullptr;
  for (const Widget& w : widgets) {
    if (w.type == "ring") {
      ring_widget = &w;  // drawn last, on top of the (opaque) character
    } else if (w.type == "character") {
      anim_img = lv_image_create(scr);
      lv_obj_set_pos(anim_img, w.x, w.y);
      lv_obj_set_size(anim_img, w.w, w.h);
      lv_obj_add_flag(anim_img, LV_OBJ_FLAG_HIDDEN);
    } else if (w.type == "clock") {
      clock_label = label(scr, "", font(w.font), theme.text, w.x, w.y, w.w, w.h);
    } else if (w.type == "date") {
      date_label = label(scr, "", font(w.font), theme.muted, w.x, w.y, w.w, w.h);
    } else if (w.type == "status") {
      status_label = label(scr, "", font(w.font), theme.text, w.x, w.y, w.w, w.h, w.lines);
    }
  }
  if (ring_widget) {
    ring = lv_obj_create(scr);
    lv_obj_set_size(ring, board::SCREEN_SIZE, board::SCREEN_SIZE);
    lv_obj_set_pos(ring, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, ring_widget->thickness, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  }
  create_touch_layer(scr);  // last, so it is on top
  update_clock();
  apply_state();
}

void setCharacter(const CharacterSet& set) {
  Lock l;
  character = set;  // the player keeps its own reference to the blob it is showing until it switches
  apply_state();
}

void setState(const char* state) {
  Lock l;
  if (current_state == state) return;
  current_state = state;
  apply_state();
}

const char* state() { return current_state.c_str(); }

void setMuted(bool m) {
  Lock l;
  if (muted == m) return;
  muted = m;
  apply_state();
}

void onTouch(void (*on_tap)(), int (*on_volume)(int delta)) {
  Lock l;
  tap_cb = on_tap;
  volume_cb = on_volume;
}

void setStatusText(const char* text) {
  Lock l;
  status_text = text;
  apply_state();
}

void identify() {
  Lock l;
  if (flash) return;
  flash = lv_obj_create(lv_screen_active());
  lv_obj_set_size(flash, board::SCREEN_SIZE, board::SCREEN_SIZE);
  lv_obj_set_pos(flash, 0, 0);
  lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
  lv_obj_set_style_border_width(flash, 0, 0);
  flash_timer = lv_timer_create(
      [](lv_timer_t* t) {
        uint32_t* ticks = static_cast<uint32_t*>(lv_timer_get_user_data(t));
        if (++*ticks >= 12) {
          lv_obj_delete(flash);
          flash = nullptr;
          lv_timer_delete(t);
          flash_timer = nullptr;
          delete ticks;
          return;
        }
        if (*ticks % 2) lv_obj_add_flag(flash, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(flash, LV_OBJ_FLAG_HIDDEN);
      },
      250, new uint32_t(0));
}

}  // namespace ui
