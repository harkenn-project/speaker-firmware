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
String offer_version;  // non-empty while an update is offered
void (*offer_install)() = nullptr;
void (*offer_dismiss)() = nullptr;
lv_obj_t* offer_panel = nullptr;
lv_obj_t* progress_bar = nullptr;
lv_obj_t* progress_detail = nullptr;
String progress_title;

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

// The assistant's screen: a picture over the home screen with tappable areas (showScreen).
lv_obj_t* screen_layer = nullptr;
lv_obj_t* screen_press = nullptr;  // highlights the area being pressed
std::vector<TapRegion> screen_regions;
bool screen_tap_dismisses = true;
void (*screen_tap_cb)(const char* label) = nullptr;
struct ScreenGesture {
  int region = -1;  // the area pressed, -1 = none
  bool cancelled = false;
  lv_point_t start{};
} screen_gesture;

lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

// KANI animation player (format: app/kani.py in server-docker). Frames are decoded into one RGB565 frame buffer in PSRAM
// that an lv_image displays; frame 0 is complete, later frames only carry changed pixels.

uint16_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

struct Player {
  lv_obj_t* img = nullptr;  // the widget it plays in
  std::shared_ptr<Blob> blob;
  uint16_t w = 0, h = 0, frames = 0, index = 0;
  uint16_t* fb = nullptr;
  size_t fb_pixels = 0;
  lv_image_dsc_t dsc{};
  lv_timer_t* timer = nullptr;
};
Player player;         // the character
Player screen_player;  // the assistant's screen

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

void player_stop(Player& p) {
  if (p.timer) lv_timer_delete(p.timer);
  p.timer = nullptr;
  p.blob.reset();
  p.img = nullptr;
}

void player_tick(lv_timer_t* t) {
  Player& p = *static_cast<Player*>(lv_timer_get_user_data(t));
  if (!p.blob || !p.img) return;
  p.index = (p.index + 1) % p.frames;
  if (!kani_decode(*p.blob, p.index, p.fb, p.fb_pixels)) {
    player_stop(p);
    return;
  }
  lv_image_cache_drop(&p.dsc);
  lv_obj_invalidate(p.img);
  lv_timer_set_period(t, kani_frame_ms(*p.blob, p.index));
}

void player_play(Player& p, lv_obj_t* img, const std::shared_ptr<Blob>& blob) {
  if (!img || (img == p.img && blob == p.blob)) return;
  player_stop(p);
  uint16_t w, h, n;
  if (!blob || !kani_valid(*blob, w, h, n)) {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const size_t pixels = static_cast<size_t>(w) * h;
  if (pixels != p.fb_pixels) {
    heap_caps_free(p.fb);
    p.fb = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM));
    p.fb_pixels = p.fb ? pixels : 0;
    if (!p.fb) return;
  }
  if (!kani_decode(*blob, 0, p.fb, pixels)) {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  p.img = img;
  p.blob = blob;
  p.w = w;
  p.h = h;
  p.frames = n;
  p.index = 0;
  p.dsc = {};
  p.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  p.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  p.dsc.header.w = w;
  p.dsc.header.h = h;
  p.dsc.header.stride = w * 2;
  p.dsc.data = reinterpret_cast<const uint8_t*>(p.fb);
  p.dsc.data_size = pixels * 2;
  lv_image_cache_drop(&p.dsc);
  lv_image_set_src(img, &p.dsc);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(img);
  if (n > 1) p.timer = lv_timer_create(player_tick, kani_frame_ms(*blob, 0), &p);
}

const lv_font_t* font(const String& size) {
  if (size == "sm") return &lv_font_montserrat_14;
  if (size == "lg") return &lv_font_montserrat_28;
  if (size == "xl") return &lv_font_montserrat_48;
  return &lv_font_montserrat_18;
}

void forget_media_button();
void add_media_button(lv_obj_t* scr);

// Removes everything from the screen and paints the theme background.
lv_obj_t* reset_screen() {
  lv_obj_t* scr = lv_screen_active();
  player_stop(player);
  player_stop(screen_player);
  lv_obj_clean(scr);
  ring = anim_img = clock_label = date_label = status_label = flash = nullptr;
  touch_layer = volume_arc = volume_label = nullptr;
  progress_bar = progress_detail = offer_panel = nullptr;
  screen_layer = screen_press = nullptr;
  forget_media_button();
  screen_regions.clear();
  progress_title = "";
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
  if (state == "speaking" || state == "ringing") return theme.accent;
  if (state == "error") return 0xff5d5d;
  if (state == "sleeping") return theme.background;
  return theme.surface;
}

const char* default_status(const String& state) {
  if (state == "listening") return "Listening...";
  if (state == "thinking") return "Thinking...";
  if (state == "error") return "Something went wrong";
  if (state == "muted") return "Mic off";
  if (state == "ringing") return "Tap to answer";
  return "";
}

// The "clock" sleep display: a big clock over everything (on the top layer, so a new layout doesn't remove it).
lv_obj_t* sleep_panel = nullptr;
lv_obj_t* sleep_clock = nullptr;

void update_clock(lv_timer_t* = nullptr) {
  if (!clock_label && !date_label && !sleep_clock) return;
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
  if (sleep_clock) {
    if (!valid) strcpy(buf, "--:--");
    else if (clock_24h) strftime(buf, sizeof(buf), "%H:%M", &t);
    else snprintf(buf, sizeof(buf), "%d:%02d", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min);
    lv_label_set_text(sleep_clock, buf);
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
    player_play(player, anim_img, character[i]);
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

// Update prompt

lv_obj_t* offer_button(lv_obj_t* parent, const char* text, uint32_t bg, uint32_t fg, int x, void (*cb)()) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, 84, 38);
  lv_obj_set_pos(b, x, 78);
  lv_obj_set_style_bg_color(b, color(bg), 0);
  lv_obj_set_style_radius(b, 19, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, color(fg), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t* e) {
        auto fn = reinterpret_cast<void (*)()>(lv_event_get_user_data(e));
        offer_version = "";
        if (offer_panel) {
          lv_obj_delete_async(offer_panel);  // we're inside one of its buttons' event handlers
          offer_panel = nullptr;
        }
        if (fn) fn();
      },
      LV_EVENT_CLICKED, reinterpret_cast<void*>(cb));
  return b;
}

void create_offer(lv_obj_t* scr) {
  if (offer_panel || !offer_version.length()) return;
  offer_panel = lv_obj_create(scr);
  lv_obj_set_size(offer_panel, 200, 128);
  lv_obj_set_pos(offer_panel, 20, 56);
  lv_obj_set_style_bg_color(offer_panel, color(theme.surface), 0);
  lv_obj_set_style_bg_opa(offer_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(offer_panel, color(theme.primary), 0);
  lv_obj_set_style_border_width(offer_panel, 2, 0);
  lv_obj_set_style_radius(offer_panel, 20, 0);
  lv_obj_set_style_pad_all(offer_panel, 0, 0);
  lv_obj_remove_flag(offer_panel, LV_OBJ_FLAG_SCROLLABLE);
  label(offer_panel, "Update available", &lv_font_montserrat_18, theme.text, 0, 12, 196, 24);
  const String v = "v" + offer_version;
  label(offer_panel, v.c_str(), &lv_font_montserrat_24, theme.primary, 0, 40, 196, 30);
  offer_button(offer_panel, "Install", theme.primary, theme.background, 12, offer_install);
  offer_button(offer_panel, "Dismiss", theme.background, theme.muted, 100, offer_dismiss);
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

int region_at(const lv_point_t& p) {
  for (int i = static_cast<int>(screen_regions.size()) - 1; i >= 0; i--) {  // the last drawn is on top
    const TapRegion& r = screen_regions[i];
    if (p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h) return i;
  }
  return -1;
}

void screen_event(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  ScreenGesture& g = screen_gesture;
  if (code == LV_EVENT_PRESSED) {
    g = ScreenGesture{};
    g.start = p;
    g.region = region_at(p);
    if (g.region >= 0 && screen_press) {
      const TapRegion& r = screen_regions[g.region];
      lv_obj_set_pos(screen_press, r.x, r.y);
      lv_obj_set_size(screen_press, r.w, r.h);
      lv_obj_remove_flag(screen_press, LV_OBJ_FLAG_HIDDEN);
    }
  } else if (code == LV_EVENT_PRESSING) {
    if (!g.cancelled && abs(p.x - g.start.x) + abs(p.y - g.start.y) > 24) {
      g.cancelled = true;  // a swipe, not a tap
      if (screen_press) lv_obj_add_flag(screen_press, LV_OBJ_FLAG_HIDDEN);
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (screen_press) lv_obj_add_flag(screen_press, LV_OBJ_FLAG_HIDDEN);
    if (code == LV_EVENT_PRESS_LOST || g.cancelled || !screen_tap_cb) return;
    if (g.region >= 0) screen_tap_cb(screen_regions[g.region].label.c_str());
    else if (screen_tap_dismisses) screen_tap_cb("");
  }
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

void showWifiSetup(const char* title, const char* apName, bool secured) {
  Lock l;
  lv_obj_t* scr = reset_screen();
  label(scr, title, &lv_font_montserrat_18, theme.text, 40, 36, 160, 24);
  label(scr, "Join the network", &lv_font_montserrat_14, theme.muted, 30, 70, 180, 18);
  label(scr, apName, &lv_font_montserrat_24, theme.primary, 14, 90, 212, 30);
  label(scr, "then open", &lv_font_montserrat_14, theme.muted, 40, 128, 160, 18);
  label(scr, "192.168.4.1", &lv_font_montserrat_18, theme.text, 40, 146, 160, 24);
  if (secured) label(scr, "Wi-Fi password: the speaker password", &lv_font_montserrat_14, theme.muted, 40, 176, 160, 36, 2);
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
  label(scr, "Enter it in Harkenn", &lv_font_montserrat_14, theme.muted, 30, 150, 180, 20);
  label(scr, host, &lv_font_montserrat_14, theme.muted, 20, 170, 200, 20);
}

void showProgress(const char* title, const char* detail, int percent) {
  Lock l;
  if (!progress_bar || progress_title != title) {
    lv_obj_t* scr = reset_screen();
    progress_title = title;
    label(scr, title, &lv_font_montserrat_18, theme.text, 26, 78, 188, 24);
    progress_bar = lv_bar_create(scr);
    lv_obj_set_size(progress_bar, 160, 14);
    lv_obj_set_pos(progress_bar, 40, 114);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_obj_set_style_bg_color(progress_bar, color(theme.surface), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, color(theme.primary), LV_PART_INDICATOR);
    progress_detail = label(scr, "", &lv_font_montserrat_14, theme.muted, 30, 138, 180, 20);
  }
  lv_bar_set_value(progress_bar, constrain(percent, 0, 100), LV_ANIM_OFF);
  lv_label_set_text(progress_detail, detail);
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
  create_touch_layer(scr);  // on top of the layout
  add_media_button(scr);    // the media player's button (with an SD card in)
  create_offer(scr);        // and the update prompt above that
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

void showSleepClock(bool on) {
  Lock l;
  if (on && !sleep_panel) {
    sleep_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sleep_panel);
    lv_obj_remove_flag(sleep_panel, LV_OBJ_FLAG_CLICKABLE);  // taps reach the home screen: sleep only changes the look
    lv_obj_set_size(sleep_panel, 240, 240);
    lv_obj_set_style_bg_color(sleep_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sleep_panel, LV_OPA_COVER, 0);
    lv_obj_move_background(sleep_panel);  // under the remote icon
    sleep_clock = lv_label_create(sleep_panel);
    lv_obj_set_style_text_font(sleep_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(sleep_clock, color(theme.text), 0);
    lv_obj_center(sleep_clock);
    update_clock();
  } else if (!on && sleep_panel) {
    lv_obj_delete(sleep_panel);
    sleep_panel = sleep_clock = nullptr;
  }
}

namespace {

// The status row, at the top of every screen (on the top layer): remote, alarm, notification, and the next timer's
// time left. Each shows only when it applies.
lv_obj_t* status_row = nullptr;
lv_obj_t* status_remote = nullptr;
lv_obj_t* status_alarm = nullptr;
lv_obj_t* status_note = nullptr;
lv_obj_t* status_timer = nullptr;
void (*on_note_tap)() = nullptr;

lv_obj_t* status_icon(const char* symbol, uint32_t rgb) {
  lv_obj_t* l = lv_label_create(status_row);
  lv_label_set_text(l, symbol);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, color(rgb), 0);
  lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
  return l;
}

void make_status_row() {
  if (status_row) return;
  status_row = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(status_row);
  lv_obj_set_size(status_row, 140, 22);
  lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status_row, 8, 0);
  lv_obj_remove_flag(status_row, LV_OBJ_FLAG_CLICKABLE);
  status_remote = status_icon(LV_SYMBOL_SHUFFLE, 0xffb547);
  status_alarm = status_icon(LV_SYMBOL_BELL, 0xe8ecff);
  status_note = status_icon(LV_SYMBOL_ENVELOPE, 0x7aa2ff);
  lv_obj_add_flag(status_note, LV_OBJ_FLAG_CLICKABLE);  // reopens the notification
  lv_obj_set_ext_click_area(status_note, 14);
  lv_obj_add_event_cb(status_note, [](lv_event_t*) { if (on_note_tap) on_note_tap(); }, LV_EVENT_CLICKED, nullptr);
  status_timer = status_icon("", 0xe8ecff);
}

void show_if(lv_obj_t* o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Animation helpers: they only move or fade objects, so nothing needs an extra drawing buffer.
void anim_y(void* o, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(o), v); }
void anim_x(void* o, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(o), v); }
void anim_opa(void* o, int32_t v) { lv_obj_set_style_opa(static_cast<lv_obj_t*>(o), v, 0); }
void anim_bg_opa(void* o, int32_t v) { lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(o), v, 0); }
void anim_size(void* o, int32_t v) {
  lv_obj_set_size(static_cast<lv_obj_t*>(o), v, v);
  lv_obj_center(static_cast<lv_obj_t*>(o));
}

void animate(lv_obj_t* o, lv_anim_exec_xcb_t fn, int32_t from, int32_t to, uint32_t ms, int repeat = 0,
             bool back = false, lv_anim_path_cb_t path = lv_anim_path_ease_in_out, uint32_t delay = 0) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, fn);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_delay(&a, delay);
  lv_anim_set_path_cb(&a, path);
  if (back) lv_anim_set_playback_duration(&a, ms);
  if (repeat) lv_anim_set_repeat_count(&a, repeat < 0 ? LV_ANIM_REPEAT_INFINITE : repeat);
  lv_anim_start(&a);
}

lv_obj_t* full_panel() {  // over everything, under the status row
  lv_obj_t* p = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, 240, 240);
  lv_obj_set_style_bg_color(p, color(theme.background), 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  if (status_row) lv_obj_move_foreground(status_row);
  return p;
}

lv_obj_t* round_button(lv_obj_t* parent, const char* text, uint32_t bg, uint32_t fg, int x, int y, int w,
                       lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, 40);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_color(b, color(bg), 0);
  lv_obj_set_style_radius(b, 20, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, color(fg), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

// The countdown: a ring that drains as the timer runs, the time left and the label. A tap hides it.
lv_obj_t* countdown_panel = nullptr;
lv_obj_t* countdown_arc = nullptr;
lv_obj_t* countdown_time = nullptr;
lv_timer_t* countdown_timer = nullptr;
time_t countdown_due = 0;
uint32_t countdown_total = 1, countdown_hide_at = 0;

void format_left(char* out, size_t n, long left) {
  if (left >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", left / 3600, left / 60 % 60, left % 60);
  else snprintf(out, n, "%ld:%02ld", left / 60, left % 60);
}

void hide_countdown_locked() {
  if (countdown_timer) lv_timer_delete(countdown_timer);
  countdown_timer = nullptr;
  if (countdown_panel) lv_obj_delete_async(countdown_panel);
  countdown_panel = countdown_arc = countdown_time = nullptr;
}

void tick_countdown(lv_timer_t*) {
  if (!countdown_panel) return;
  const long left = max<long>(0, static_cast<long>(countdown_due - time(nullptr)));
  char buf[16];
  format_left(buf, sizeof(buf), left);
  lv_label_set_text(countdown_time, buf);
  lv_arc_set_value(countdown_arc, static_cast<int32_t>(1000L * left / max<uint32_t>(1, countdown_total)));
  if (left <= 10) lv_obj_set_style_text_color(countdown_time, color(left % 2 ? theme.accent : theme.text), 0);
  if (!left || static_cast<int32_t>(millis() - countdown_hide_at) >= 0) hide_countdown_locked();
}

// Ringing: a bell that shakes inside a pulsing halo, what's ringing, and Stop / Snooze. A tap anywhere else stops it.
lv_obj_t* ringing_panel = nullptr;
void (*ringing_stop)() = nullptr;
void (*ringing_snooze)() = nullptr;

void close_ringing(void (*fn)()) {
  if (ringing_panel) lv_obj_delete_async(ringing_panel);  // we may be inside one of its handlers
  ringing_panel = nullptr;
  if (fn) fn();
}

// A notification: a card that drops in with a bouncing envelope, then rises away; the envelope stays in the status
// row until someone looks at it.
lv_obj_t* note_card = nullptr;
lv_timer_t* note_timer = nullptr;
void (*note_seen)() = nullptr;
uint32_t note_hide_at = 0, note_gone_at = 0;  // when it starts rising away, and when it's gone

void close_note() {
  if (note_timer) lv_timer_delete(note_timer);
  note_timer = nullptr;
  if (note_card) lv_obj_delete_async(note_card);
  note_card = nullptr;
}

}  // namespace

void setStatusIcons(bool remote, bool alarm, bool note, const char* timer_text) {
  Lock l;
  make_status_row();
  show_if(status_remote, remote);
  show_if(status_alarm, alarm);
  const bool was_note = !lv_obj_has_flag(status_note, LV_OBJ_FLAG_HIDDEN);
  show_if(status_note, note);
  if (note && !was_note) animate(status_note, anim_opa, LV_OPA_20, LV_OPA_COVER, 450, 3, true);  // it arrived
  const bool timer = timer_text && timer_text[0];
  if (timer) lv_label_set_text(status_timer, (String(LV_SYMBOL_LOOP " ") + timer_text).c_str());
  show_if(status_timer, timer);
}

void onNotificationTap(void (*fn)()) {
  Lock l;
  on_note_tap = fn;
}

void setRemote(bool on) {
  Lock l;
  make_status_row();
  show_if(status_remote, on);
}

void showCountdown(const char* name, time_t due, uint32_t total_s, uint32_t seconds) {
  Lock l;
  hide_countdown_locked();
  countdown_due = due;
  countdown_total = max<uint32_t>(1, total_s);
  countdown_hide_at = millis() + seconds * 1000;
  countdown_panel = full_panel();
  lv_obj_add_flag(countdown_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(countdown_panel, [](lv_event_t*) { hide_countdown_locked(); }, LV_EVENT_CLICKED, nullptr);
  countdown_arc = lv_arc_create(countdown_panel);
  lv_obj_set_size(countdown_arc, 220, 220);
  lv_obj_center(countdown_arc);
  lv_arc_set_rotation(countdown_arc, 270);  // from the top, clockwise
  lv_arc_set_bg_angles(countdown_arc, 0, 360);
  lv_arc_set_range(countdown_arc, 0, 1000);
  lv_obj_remove_flag(countdown_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(countdown_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(countdown_arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(countdown_arc, color(theme.surface), LV_PART_MAIN);
  lv_obj_set_style_arc_color(countdown_arc, color(theme.primary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(countdown_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(countdown_arc, 0, LV_PART_KNOB);
  countdown_time = label(countdown_panel, "", &lv_font_montserrat_48, theme.text, 20, 86, 200, 60);
  label(countdown_panel, name && name[0] ? name : "Timer", &lv_font_montserrat_18, theme.muted, 40, 148, 160, 24);
  animate(countdown_arc, anim_opa, LV_OPA_TRANSP, LV_OPA_COVER, 400);  // fades in
  countdown_timer = lv_timer_create(tick_countdown, 250, nullptr);
  tick_countdown(nullptr);
}

void hideCountdown() {
  Lock l;
  hide_countdown_locked();
}

void showRinging(const char* title, const char* detail, const char* snooze_text, void (*on_stop)(),
                 void (*on_snooze)()) {
  Lock l;
  if (ringing_panel) lv_obj_delete(ringing_panel);
  ringing_stop = on_stop;
  ringing_snooze = on_snooze;
  ringing_panel = full_panel();
  lv_obj_add_flag(ringing_panel, LV_OBJ_FLAG_CLICKABLE);  // a tap anywhere else stops it
  lv_obj_add_event_cb(ringing_panel, [](lv_event_t*) { close_ringing(ringing_stop); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* halo = lv_obj_create(ringing_panel);  // a pulsing glow behind the bell
  lv_obj_remove_style_all(halo);
  lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(halo, color(theme.accent), 0);
  lv_obj_set_style_bg_opa(halo, LV_OPA_30, 0);
  lv_obj_remove_flag(halo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(halo, 80, 80);
  lv_obj_align(halo, LV_ALIGN_TOP_MID, 0, 34);
  animate(halo, anim_bg_opa, LV_OPA_10, LV_OPA_50, 500, -1, true);
  lv_obj_t* bell = label(ringing_panel, LV_SYMBOL_BELL, &lv_font_montserrat_48, theme.text, 90, 44, 60, 60);
  animate(bell, anim_x, 84, 96, 90, -1, true, lv_anim_path_linear);  // it shakes
  label(ringing_panel, title, &lv_font_montserrat_24, theme.text, 20, 118, 200, 30);
  if (detail && detail[0]) label(ringing_panel, detail, &lv_font_montserrat_14, theme.muted, 30, 148, 180, 20);
  round_button(ringing_panel, "Stop", theme.primary, theme.background, on_snooze ? 34 : 70, 176,
               on_snooze ? 80 : 100, [](lv_event_t*) { close_ringing(ringing_stop); });
  if (on_snooze) {
    round_button(ringing_panel, snooze_text, theme.surface, theme.text, 126, 176, 80,
                 [](lv_event_t*) { close_ringing(ringing_snooze); });
  }
}

void hideRinging() {
  Lock l;
  if (ringing_panel) lv_obj_delete(ringing_panel);
  ringing_panel = nullptr;
}

bool ringingShown() { return ringing_panel != nullptr; }

void showNotification(const char* title, const char* text, uint32_t seconds, void (*on_seen)()) {
  Lock l;
  close_note();
  note_seen = on_seen;
  note_card = lv_obj_create(lv_layer_top());
  lv_obj_set_size(note_card, 200, 150);
  lv_obj_set_style_bg_color(note_card, color(theme.surface), 0);
  lv_obj_set_style_bg_opa(note_card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(note_card, color(theme.primary), 0);
  lv_obj_set_style_border_width(note_card, 2, 0);
  lv_obj_set_style_radius(note_card, 24, 0);
  lv_obj_set_style_pad_all(note_card, 0, 0);
  lv_obj_remove_flag(note_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_x(note_card, 20);
  if (status_row) lv_obj_move_foreground(status_row);
  lv_obj_add_event_cb(note_card, [](lv_event_t*) {
    close_note();
    if (note_seen) note_seen();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* icon = label(note_card, LV_SYMBOL_ENVELOPE, &lv_font_montserrat_28, theme.primary, 0, 8, 196, 34);
  label(note_card, title && title[0] ? title : "Notification", &lv_font_montserrat_18, theme.text, 10, 44, 176, 24);
  label(note_card, text, &lv_font_montserrat_14, theme.muted, 12, 70, 172, 66, 4);
  animate(note_card, anim_y, -160, 44, 520, 0, false, lv_anim_path_overshoot);  // drops in
  animate(icon, anim_y, 4, 12, 260, 4, true, lv_anim_path_ease_out, 520);       // and the envelope bounces
  note_hide_at = millis() + max<uint32_t>(3, seconds) * 1000;
  note_gone_at = 0;
  note_timer = lv_timer_create([](lv_timer_t*) {
    if (!note_gone_at && static_cast<int32_t>(millis() - note_hide_at) >= 0) {
      animate(note_card, anim_y, lv_obj_get_y(note_card), -170, 420, 0, false, lv_anim_path_ease_in);  // rises away
      note_gone_at = millis() + 450;
    } else if (note_gone_at && static_cast<int32_t>(millis() - note_gone_at) >= 0) {
      close_note();  // deletes this timer: LVGL allows that from its own callback
    }
  }, 100, nullptr);
}

namespace {
lv_obj_t* notice_panel = nullptr;
lv_timer_t* notice_timer = nullptr;
void (*notice_taps[2])() = {nullptr, nullptr};
uint32_t notice_hide_at = 0;

void close_notice() {
  if (notice_timer) lv_timer_delete(notice_timer);
  notice_timer = nullptr;
  if (notice_panel) lv_obj_delete_async(notice_panel);
  notice_panel = nullptr;
}

void anim_translate_y(void* o, int32_t v) { lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(o), v, 0); }

lv_obj_t* notice_button(const char* text, int index, bool primary, uint32_t tint, int x, int w, int y) {
  lv_obj_t* b = lv_button_create(notice_panel);
  lv_obj_set_size(b, w, 36);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_color(b, color(primary ? tint : theme.background), 0);
  lv_obj_set_style_radius(b, 18, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, color(primary ? theme.background : theme.muted), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t* e) {
        void (*fn)() = notice_taps[reinterpret_cast<intptr_t>(lv_event_get_user_data(e))];
        close_notice();  // deleted later: we're inside its button's handler
        if (fn) fn();
      },
      LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
  return b;
}
}  // namespace

void showNotice(const char* icon, const char* title, const char* text, NoticeKind kind, uint32_t seconds,
                NoticeButton primary, NoticeButton secondary) {
  Lock l;
  close_notice();
  const uint32_t tint = kind == NoticeKind::Good ? 0x6be3a4 : kind == NoticeKind::Warning ? 0xffb547 : theme.primary;
  const bool buttons = primary.label != nullptr;
  notice_taps[0] = primary.on_tap;
  notice_taps[1] = secondary.on_tap;
  const int w = 204, h = buttons ? 172 : 126;
  notice_panel = lv_obj_create(lv_layer_top());
  lv_obj_set_size(notice_panel, w, h);
  lv_obj_align(notice_panel, LV_ALIGN_CENTER, 0, buttons ? 4 : 0);
  lv_obj_set_style_bg_color(notice_panel, color(theme.surface), 0);
  lv_obj_set_style_bg_opa(notice_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(notice_panel, 0, 0);
  lv_obj_set_style_radius(notice_panel, 26, 0);
  lv_obj_set_style_shadow_width(notice_panel, 24, 0);
  lv_obj_set_style_shadow_color(notice_panel, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(notice_panel, LV_OPA_50, 0);
  lv_obj_set_style_pad_all(notice_panel, 0, 0);
  lv_obj_remove_flag(notice_panel, LV_OBJ_FLAG_SCROLLABLE);
  if (status_row) lv_obj_move_foreground(status_row);

  lv_obj_t* badge = lv_obj_create(notice_panel);  // the icon in a soft, tinted circle
  lv_obj_set_size(badge, 40, 40);
  lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(badge, color(tint), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_30, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* glyph = lv_label_create(badge);
  lv_label_set_text(glyph, icon && *icon ? icon : LV_SYMBOL_BELL);
  lv_obj_set_style_text_color(glyph, color(tint), 0);
  lv_obj_set_style_text_font(glyph, &lv_font_montserrat_18, 0);
  lv_obj_center(glyph);

  label(notice_panel, title, &lv_font_montserrat_18, theme.text, 10, 58, w - 20, 24);
  label(notice_panel, text, &lv_font_montserrat_14, theme.muted, 14, 82, w - 28, 36, 2);
  if (buttons) {
    if (secondary.label) {
      notice_button(primary.label, 0, true, tint, 12, 86, 124);
      notice_button(secondary.label, 1, false, tint, w - 98, 86, 124);
    } else {
      notice_button(primary.label, 0, true, tint, (w - 120) / 2, 120, 124);
    }
  }
  // pops up: fades in as it rises a little into place
  animate(notice_panel, anim_opa, LV_OPA_TRANSP, LV_OPA_COVER, 220);
  animate(notice_panel, anim_translate_y, 18, 0, 320, 0, false, lv_anim_path_ease_out);
  animate(badge, anim_opa, LV_OPA_TRANSP, LV_OPA_COVER, 300, 0, false, lv_anim_path_ease_out, 120);
  if (!buttons) {
    notice_hide_at = millis() + max<uint32_t>(2, seconds) * 1000;
    notice_timer = lv_timer_create([](lv_timer_t*) {
      if (static_cast<int32_t>(millis() - notice_hide_at) < 0 || !notice_panel) return;
      animate(notice_panel, anim_opa, LV_OPA_COVER, LV_OPA_TRANSP, 260);  // and fades away
      notice_hide_at = millis() + 100000;
      lv_timer_t* gone = lv_timer_create([](lv_timer_t* t) {
        lv_timer_delete(t);
        close_notice();
      }, 300, nullptr);
      lv_timer_set_repeat_count(gone, 1);
    }, 100, nullptr);
  }
}

void hideNotice() {
  Lock l;
  close_notice();
}

void showUpdateOffer(const char* version, void (*on_install)(), void (*on_dismiss)()) {
  Lock l;
  if (offer_panel) lv_obj_delete(offer_panel);
  offer_panel = nullptr;
  offer_version = version;
  offer_install = on_install;
  offer_dismiss = on_dismiss;
  if (touch_layer) create_offer(lv_screen_active());  // otherwise it appears with the home screen
}

void hideUpdateOffer() {
  Lock l;
  offer_version = "";
  if (offer_panel) lv_obj_delete(offer_panel);
  offer_panel = nullptr;
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

namespace ui {

void showScreen(const std::shared_ptr<Blob>& image, const std::vector<TapRegion>& regions, bool tap_dismisses) {
  Lock l;
  if (!touch_layer) return;  // only over the home screen
  if (screen_layer) {
    player_stop(screen_player);
    lv_obj_delete(screen_layer);
  }
  screen_regions = regions;
  screen_tap_dismisses = tap_dismisses;
  lv_obj_t* scr = lv_screen_active();
  screen_layer = lv_obj_create(scr);
  lv_obj_remove_style_all(screen_layer);
  lv_obj_set_size(screen_layer, board::SCREEN_SIZE, board::SCREEN_SIZE);
  lv_obj_set_pos(screen_layer, 0, 0);
  lv_obj_set_style_bg_color(screen_layer, color(theme.background), 0);
  lv_obj_set_style_bg_opa(screen_layer, LV_OPA_COVER, 0);
  lv_obj_remove_flag(screen_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(screen_layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(screen_layer, screen_event, LV_EVENT_ALL, nullptr);
  lv_obj_t* img = lv_image_create(screen_layer);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(img, 0, 0);
  lv_obj_set_size(img, board::SCREEN_SIZE, board::SCREEN_SIZE);
  player_play(screen_player, img, image);
  screen_press = lv_obj_create(screen_layer);
  lv_obj_remove_style_all(screen_press);
  lv_obj_remove_flag(screen_press, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(screen_press, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(screen_press, LV_OPA_30, 0);
  lv_obj_set_style_radius(screen_press, 12, 0);
  lv_obj_add_flag(screen_press, LV_OBJ_FLAG_HIDDEN);
  if (ring) lv_obj_move_foreground(ring);  // the state ring stays visible (it doesn't take touches)
  if (offer_panel) lv_obj_move_foreground(offer_panel);
}

void hideScreen() {
  Lock l;
  if (!screen_layer) return;
  player_stop(screen_player);
  lv_obj_delete(screen_layer);
  screen_layer = screen_press = nullptr;
  screen_regions.clear();
}

bool screenShown() {
  Lock l;
  return screen_layer != nullptr;
}

void onScreenTap(void (*on_tap)(const char* label)) {
  Lock l;
  screen_tap_cb = on_tap;
}

}  // namespace ui

// The media player on the screen: with an SD card in, a button on the home screen opens Media (Music, Soundscapes,
// Meditations), lists, and the player, so the card plays without talking to anyone. Drawn here on the speaker (not by
// Harkenn) so it answers taps at once and works without Wi-Fi, like the card it plays.

namespace ui {
namespace {

enum class View : uint8_t { None, Home, List, Tracks, Player };
constexpr uint32_t MEDIA_IDLE_MS = 45000;  // the media screens close after this long without a touch

bool media_card = false;
MediaLibrary media_lib;
MediaNow media_now;
MediaHandlers media_do;
lv_obj_t* media_panel = nullptr;   // the open view, over everything
lv_timer_t* media_timer = nullptr;
View media_view = View::None;
String list_kind;                  // "music", "soundscape", "meditation"
String list_album;                 // for Tracks
std::vector<String> list_names;    // what the open list shows

// the player's parts, updated by setMediaNow
lv_obj_t* p_arc = nullptr;
lv_obj_t* p_kind = nullptr;
lv_obj_t* p_title = nullptr;
lv_obj_t* p_sub = nullptr;
lv_obj_t* p_play = nullptr;
lv_obj_t* p_repeat = nullptr;
lv_obj_t* p_sleep = nullptr;

void open_view(View v);

String clock_text(uint32_t s) {
  char t[16];
  if (s >= 3600) snprintf(t, sizeof(t), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(t, sizeof(t), "%u:%02u", s / 60, s % 60);
  return t;
}

const char* kind_word(const String& kind) {
  return kind == "soundscape" ? "Soundscapes" : kind == "meditation" ? "Meditations" : "Music";
}

void close_media() {
  if (media_timer) lv_timer_delete(media_timer);
  media_timer = nullptr;
  if (media_panel) lv_obj_delete_async(media_panel);
  media_panel = p_arc = p_kind = p_title = p_sub = p_play = p_repeat = p_sleep = nullptr;
  media_view = View::None;
}

// A round icon button; `cb` gets `data` as its user data.
lv_obj_t* icon_button(lv_obj_t* parent, const char* text, int x, int y, int size, bool filled, lv_event_cb_t cb,
                      void* data = nullptr) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, color(filled ? theme.primary : theme.surface), 0);
  lv_obj_set_style_bg_color(b, color(filled ? theme.text : theme.muted), LV_STATE_PRESSED);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, color(filled ? theme.background : theme.text), 0);
  lv_obj_set_style_text_font(l, size >= 56 ? &lv_font_montserrat_24 : &lv_font_montserrat_18, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
  return b;
}

// A wide pill in a list: an icon, the name (cut short with "..."), and a muted detail on the right.
lv_obj_t* pill(lv_obj_t* parent, const char* icon, const char* name, const char* detail, lv_event_cb_t cb, intptr_t i) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, lv_pct(100), 42);
  lv_obj_set_style_radius(b, 21, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, color(theme.surface), 0);
  lv_obj_set_style_bg_color(b, color(theme.primary), LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(b, 14, 0);
  lv_obj_t* ic = lv_label_create(b);
  lv_label_set_text(ic, icon);
  lv_obj_set_style_text_color(ic, color(theme.primary), 0);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* n = lv_label_create(b);
  lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
  lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);  // one line, cut short with "...": needs a fixed height, or it wraps
  lv_obj_set_size(n, detail && *detail ? 104 : 124, lv_font_get_line_height(&lv_font_montserrat_14));
  lv_label_set_text(n, name);
  lv_obj_set_style_text_color(n, color(theme.text), 0);
  lv_obj_align(n, LV_ALIGN_LEFT_MID, 24, 0);
  if (detail && *detail) {
    lv_obj_t* d = lv_label_create(b);
    lv_label_set_text(d, detail);
    lv_obj_set_style_text_color(d, color(theme.muted), 0);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
  return b;
}

lv_obj_t* scroll_list(lv_obj_t* parent, int y, int h) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 176, h);
  lv_obj_set_pos(box, 32, y);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 8, 0);
  lv_obj_set_scroll_dir(box, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
  return box;
}

void back_button(lv_obj_t* parent, lv_event_cb_t cb) { icon_button(parent, LV_SYMBOL_LEFT, 98, 196, 44, false, cb); }

int32_t uptr(lv_event_t* e) { return static_cast<int32_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))); }

// Home: up to three big choices, only those with something on the card.
void build_home() {
  if (media_now.active) {  // what's playing takes the title's place: a tap goes back to it
    lv_obj_t* b = pill(media_panel, media_now.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, media_now.title.c_str(), "",
                       [](lv_event_t*) { open_view(View::Player); }, 0);
    lv_obj_set_size(b, 150, 36);
    lv_obj_set_pos(b, 45, 20);
    lv_obj_set_style_bg_color(b, color(theme.primary), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), color(theme.background), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 1), color(theme.background), 0);
  } else {
    label(media_panel, "Media", &lv_font_montserrat_24, theme.text, 0, 22, 240, 30);
  }
  struct Choice { const char* kind; const char* icon; size_t count; };
  const Choice choices[] = {{"music", LV_SYMBOL_AUDIO, media_lib.albums.size()},
                            {"soundscape", LV_SYMBOL_LOOP, media_lib.soundscapes.size()},
                            {"meditation", LV_SYMBOL_BELL, media_lib.meditations.size()}};
  int y = 64;
  bool any = false;
  for (int i = 0; i < 3; i++) {
    if (!choices[i].count) continue;
    any = true;
    char n[8];
    snprintf(n, sizeof(n), "%u", static_cast<unsigned>(choices[i].count));
    lv_obj_t* b = pill(media_panel, choices[i].icon, kind_word(choices[i].kind), n, [](lv_event_t* e) {
      static const char* kinds[] = {"music", "soundscape", "meditation"};
      list_kind = kinds[uptr(e)];
      open_view(View::List);
    }, i);
    lv_obj_set_size(b, 176, 42);
    lv_obj_set_pos(b, 32, y);
    y += 50;
  }
  if (!any) {
    label(media_panel, "Nothing on the card yet. Send some from Harkenn's media pages.", &lv_font_montserrat_14,
          theme.muted, 30, 80, 180, 80, 4);
  }
  icon_button(media_panel, LV_SYMBOL_CLOSE, 98, 196, 44, false, [](lv_event_t*) { close_media(); });
}

// A list: albums, soundscapes or meditations; or an album's tracks.
void build_list(bool tracks) {
  const char* title = tracks ? list_album.c_str() : kind_word(list_kind);
  label(media_panel, title, &lv_font_montserrat_18, theme.text, 30, 24, 180, 26);
  list_names = tracks ? (media_do.tracks ? media_do.tracks(list_album.c_str()) : std::vector<String>{})
                      : list_kind == "music" ? media_lib.albums
                      : list_kind == "soundscape" ? media_lib.soundscapes
                                                  : media_lib.meditations;
  lv_obj_t* box = scroll_list(media_panel, 56, 132);
  if (tracks && list_names.size() > 1) {
    pill(box, LV_SYMBOL_PLAY, "Play all", "", [](lv_event_t*) {
      if (media_do.play) media_do.play("music", list_album.c_str(), 0);
      open_view(View::Player);
    }, 0);
  }
  const char* icon = tracks ? LV_SYMBOL_AUDIO : list_kind == "music" ? LV_SYMBOL_LIST
                     : list_kind == "soundscape" ? LV_SYMBOL_LOOP : LV_SYMBOL_BELL;
  for (size_t i = 0; i < list_names.size(); i++) {
    char n[8] = "";
    if (tracks) snprintf(n, sizeof(n), "%u", static_cast<unsigned>(i + 1));
    pill(box, icon, list_names[i].c_str(), n, [](lv_event_t* e) {
      const size_t i = uptr(e);
      if (i >= list_names.size()) return;
      if (media_view == View::Tracks) {
        if (media_do.play) media_do.play("music", list_album.c_str(), i);
        open_view(View::Player);
      } else if (list_kind == "music") {
        list_album = list_names[i];
        open_view(View::Tracks);
      } else {
        if (media_do.play) media_do.play(list_kind.c_str(), list_names[i].c_str(), 0);
        open_view(View::Player);
      }
    }, static_cast<intptr_t>(i));
  }
  back_button(media_panel, [](lv_event_t*) { open_view(media_view == View::Tracks ? View::List : View::Home); });
}

void update_player() {
  if (media_view != View::Player || !p_title) return;
  const MediaNow& n = media_now;
  String kind = n.kind == "soundscape" ? "SOUNDSCAPE" : n.kind == "meditation" ? "MEDITATION" : "MUSIC";
  if (n.kind == "music" && n.tracks > 1) kind += "  " + String(n.track + 1) + " / " + String(n.tracks);
  lv_label_set_text(p_kind, kind.c_str());
  lv_label_set_text(p_title, n.active ? n.title.c_str() : "Stopped");
  String sub = n.kind == "music" ? n.album + "  " : String();
  sub += clock_text(n.elapsed_s);
  if (n.duration_s && n.kind != "soundscape") sub += " / " + clock_text(n.duration_s);
  lv_label_set_text(p_sub, sub.c_str());
  lv_label_set_text(lv_obj_get_child(p_play, 0), n.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  if (n.duration_s) {
    lv_obj_remove_flag(p_arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(p_arc, static_cast<int32_t>(1000ull * n.elapsed_s / n.duration_s));
  } else {
    lv_obj_add_flag(p_arc, LV_OBJ_FLAG_HIDDEN);
  }
  if (p_repeat) {
    lv_label_set_text(lv_obj_get_child(p_repeat, 0), n.repeat == "one" ? LV_SYMBOL_LOOP "1" : LV_SYMBOL_LOOP);
    lv_obj_set_style_bg_color(p_repeat, color(n.repeat == "off" ? theme.surface : theme.primary), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(p_repeat, 0), color(n.repeat == "off" ? theme.muted : theme.background), 0);
  }
  char z[12];
  if (n.sleep_s) snprintf(z, sizeof(z), "%um", static_cast<unsigned>((n.sleep_s + 59) / 60));
  else snprintf(z, sizeof(z), "Zz");
  lv_label_set_text(lv_obj_get_child(p_sleep, 0), z);
  lv_obj_set_style_bg_color(p_sleep, color(n.sleep_s ? theme.primary : theme.surface), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(p_sleep, 0), color(n.sleep_s ? theme.background : theme.muted), 0);
}

void build_player() {
  p_arc = lv_arc_create(media_panel);  // how far through, around the edge
  lv_obj_set_size(p_arc, 236, 236);
  lv_obj_set_pos(p_arc, 2, 2);
  lv_arc_set_rotation(p_arc, 270);
  lv_arc_set_bg_angles(p_arc, 0, 360);
  lv_arc_set_range(p_arc, 0, 1000);
  lv_obj_remove_flag(p_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(p_arc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(p_arc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(p_arc, color(theme.surface), LV_PART_MAIN);
  lv_obj_set_style_arc_color(p_arc, color(theme.primary), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(p_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(p_arc, 0, LV_PART_KNOB);

  p_kind = label(media_panel, "", &lv_font_montserrat_14, theme.primary, 30, 30, 180, 18);
  p_title = label(media_panel, "", &lv_font_montserrat_18, theme.text, 26, 50, 188, 44, 2);
  p_sub = label(media_panel, "", &lv_font_montserrat_14, theme.muted, 30, 96, 180, 18);
  const bool album = media_now.kind == "music" && media_now.tracks > 1;
  if (album) {
    icon_button(media_panel, LV_SYMBOL_PREV, 40, 128, 44, false, [](lv_event_t*) { if (media_do.previous) media_do.previous(); });
    icon_button(media_panel, LV_SYMBOL_NEXT, 156, 128, 44, false, [](lv_event_t*) { if (media_do.next) media_do.next(); });
  }
  p_play = icon_button(media_panel, LV_SYMBOL_PAUSE, 90, 120, 60, true, [](lv_event_t*) { if (media_do.toggle) media_do.toggle(); });
  // the second row: home, stop, repeat (music), sleep timer
  const int y = 188;
  icon_button(media_panel, LV_SYMBOL_HOME, album ? 46 : 58, y, 36, false, [](lv_event_t*) { close_media(); });
  icon_button(media_panel, LV_SYMBOL_STOP, album ? 86 : 102, y, 36, false, [](lv_event_t*) {
    if (media_do.stop) media_do.stop();
    open_view(View::Home);
  });
  p_repeat = album ? icon_button(media_panel, LV_SYMBOL_LOOP, 126, y, 36, false, [](lv_event_t*) { if (media_do.repeat) media_do.repeat(); })
                   : nullptr;
  p_sleep = icon_button(media_panel, "Zz", album ? 166 : 146, y, 36, false, [](lv_event_t*) { if (media_do.sleep) media_do.sleep(); });
  lv_obj_set_style_text_font(lv_obj_get_child(p_sleep, 0), &lv_font_montserrat_14, 0);
  update_player();
}

void open_view(View v) {
  if (v == View::None || !media_card) {
    close_media();
    return;
  }
  if (media_timer) lv_timer_delete(media_timer);
  media_timer = nullptr;
  if (media_panel) lv_obj_delete_async(media_panel);
  media_panel = p_arc = p_kind = p_title = p_sub = p_play = p_repeat = p_sleep = nullptr;
  media_view = v;
  media_panel = full_panel();
  if (v == View::Home) build_home();
  else if (v == View::List) build_list(false);
  else if (v == View::Tracks) build_list(true);
  else build_player();
  animate(media_panel, anim_opa, LV_OPA_TRANSP, LV_OPA_COVER, 160);
  board::markActivity();
  // closes itself after a while without a touch (music plays on; the home screen's button comes back to it)
  media_timer = lv_timer_create([](lv_timer_t*) {
    if (lv_display_get_inactive_time(nullptr) > MEDIA_IDLE_MS) close_media();
  }, 1000, nullptr);
}

// The media player's way in: a note in the status row at the top (the same on every layout, so it never covers the
// clock). Muted with a card in, the theme's colour and pulsing gently while something plays, still while paused.
lv_obj_t* status_media = nullptr;
bool status_media_pulsing = false;

void update_media_button() {
  make_status_row();
  if (!status_media) {
    status_media = lv_label_create(status_row);
    lv_label_set_text(status_media, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(status_media, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(status_media, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(status_media, 18);  // easy to hit, though the note is small
    lv_obj_add_event_cb(status_media, [](lv_event_t*) { open_view(media_now.active ? View::Player : View::Home); },
                        LV_EVENT_CLICKED, nullptr);
  }
  show_if(status_media, media_card);
  lv_obj_set_style_text_color(status_media, color(media_now.active ? theme.primary : theme.muted), 0);
  const bool pulse = media_card && media_now.playing;
  if (pulse != status_media_pulsing) {
    status_media_pulsing = pulse;
    lv_anim_delete(status_media, anim_opa);
    lv_obj_set_style_opa(status_media, LV_OPA_COVER, 0);
    if (pulse) animate(status_media, anim_opa, LV_OPA_COVER, LV_OPA_40, 900, -1, true);
  }
}

void create_media_button(lv_obj_t*) { update_media_button(); }

}  // namespace

void setMediaLibrary(bool card, const MediaLibrary& lib) {
  Lock l;
  media_card = card;
  media_lib = lib;
  update_media_button();
  if (!card) close_media();
  else if (media_view == View::Home || media_view == View::List) open_view(media_view);  // show what changed
}

void setMediaNow(const MediaNow& now) {
  Lock l;
  const bool was_active = media_now.active;
  const bool reshape = media_now.kind != now.kind || media_now.tracks != now.tracks || media_now.title != now.title;
  media_now = now;
  update_media_button();
  if (media_view == View::Player) {
    if (!now.active && was_active) open_view(View::Home);
    else if (reshape) open_view(View::Player);  // a different kind of thing: its own buttons
    else update_player();
  }
}

void onMedia(const MediaHandlers& h) {
  Lock l;
  media_do = h;
}

void hideMedia() {
  Lock l;
  if (media_view != View::None) close_media();
}

bool mediaShown() { return media_view != View::None; }

}  // namespace ui

namespace ui {
namespace {
void forget_media_button() {}
void add_media_button(lv_obj_t* scr) { create_media_button(scr); }
}  // namespace
}  // namespace ui
