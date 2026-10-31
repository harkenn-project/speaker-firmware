#include "ui.h"

#include <lvgl.h>
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
lv_image_dsc_t gif_dsc[STATE_COUNT];

lv_obj_t* ring = nullptr;
lv_obj_t* gif = nullptr;
lv_obj_t* clock_label = nullptr;
lv_obj_t* date_label = nullptr;
lv_obj_t* status_label = nullptr;
lv_timer_t* clock_timer = nullptr;
lv_obj_t* flash = nullptr;
lv_timer_t* flash_timer = nullptr;

lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

const lv_font_t* font(const String& size) {
  if (size == "sm") return &lv_font_montserrat_14;
  if (size == "lg") return &lv_font_montserrat_28;
  if (size == "xl") return &lv_font_montserrat_48;
  return &lv_font_montserrat_18;
}

// Removes everything from the screen and paints the theme background.
lv_obj_t* reset_screen() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_clean(scr);
  ring = gif = clock_label = date_label = status_label = flash = nullptr;
  if (flash_timer) {
    lv_timer_delete(flash_timer);
    flash_timer = nullptr;
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

uint32_t ring_color(const String& state) {
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
  if (ring) lv_obj_set_style_border_color(ring, color(ring_color(current_state)), 0);
  if (status_label) {
    lv_label_set_text(status_label, status_text.length() ? status_text.c_str() : default_status(current_state));
  }
  if (gif) {
    int i = stateIndex(current_state.c_str());
    if (i < 0 || !character[i]) i = 0;
    if (character[i]) {
      lv_gif_set_src(gif, &gif_dsc[i]);
      lv_obj_remove_flag(gif, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(gif, LV_OBJ_FLAG_HIDDEN);
    }
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
      gif = lv_gif_create(scr);
      lv_obj_set_pos(gif, w.x, w.y);
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
  update_clock();
  apply_state();
}

void setCharacter(const CharacterSet& set) {
  Lock l;
  // Point the descriptors at the new data before the old blobs are released (when `character` is replaced).
  for (size_t i = 0; i < STATE_COUNT; i++) {
    gif_dsc[i] = {};
    if (!set[i]) continue;
    gif_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
    gif_dsc[i].header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    gif_dsc[i].data = set[i]->data;
    gif_dsc[i].data_size = set[i]->size;
  }
  CharacterSet old = character;
  character = set;
  apply_state();
}

void setState(const char* state) {
  Lock l;
  if (current_state == state) return;
  current_state = state;
  apply_state();
}

const char* state() { return current_state.c_str(); }

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
