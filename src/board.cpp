#include "board.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>

namespace {

// Rotation 0-3, or 4-7 for the mirrored variants, if the image comes out rotated or mirrored.
constexpr int DISPLAY_ROTATION = 0;
constexpr int BUFFER_LINES = 40;

class Panel : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_CST816S touch_;

 public:
  Panel() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = pins::LCD_SCLK;
      cfg.pin_mosi = pins::LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = pins::LCD_DC;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = pins::LCD_CS;
      cfg.pin_rst = pins::LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = board::SCREEN_SIZE;
      cfg.panel_height = board::SCREEN_SIZE;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = pins::LCD_BL;
      cfg.invert = true;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
      cfg.x_min = 0;
      cfg.x_max = board::SCREEN_SIZE - 1;
      cfg.y_min = 0;
      cfg.y_max = board::SCREEN_SIZE - 1;
      cfg.pin_int = pins::TOUCH_INT;
      cfg.pin_rst = pins::TOUCH_RST;
      cfg.bus_shared = false;
      cfg.i2c_port = 1;
      cfg.i2c_addr = 0x15;
      cfg.pin_sda = pins::TOUCH_SDA;
      cfg.pin_scl = pins::TOUCH_SCL;
      cfg.freq = 400000;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

Panel tft;
volatile uint32_t last_touch_ms = 0;

void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels(reinterpret_cast<lgfx::rgb565_t*>(px), w * h);
  tft.endWrite();
  lv_display_flush_ready(disp);
}

void read_touch(lv_indev_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  if (tft.getTouch(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    last_touch_ms = millis();
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

}  // namespace

namespace board {

void begin() {
  pinMode(pins::AMP_EN, OUTPUT);
  digitalWrite(pins::AMP_EN, LOW);  // speaker amp off; audio.cpp switches it on while playing
  pinMode(pins::BOOT_BUTTON, INPUT_PULLUP);
  pinMode(pins::CHARGE_STATUS, INPUT_PULLUP);
  analogReadResolution(12);

  tft.init();
  tft.setRotation(DISPLAY_ROTATION);
  tft.setBrightness(0);
  tft.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t* disp = lv_display_create(SCREEN_SIZE, SCREEN_SIZE);
  const size_t bytes = SCREEN_SIZE * BUFFER_LINES * sizeof(uint16_t);
  void* buf1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void* buf2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_display_set_buffers(disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, read_touch);
  last_touch_ms = millis();
}

void setBrightness(int percent) {
  tft.setBrightness(constrain(percent, 0, 100) * 255 / 100);
}

uint32_t lastTouchMs() { return last_touch_ms; }

void markActivity() { last_touch_ms = millis(); }

int batteryPercent() {
  // The ADC sees the cell through a 1:2 divider. Linear map from 3.40 V (empty) to 4.15 V (full).
  const int mv = analogReadMilliVolts(pins::BATTERY_ADC) * 2;
  return constrain((mv - 3400) * 100 / (4150 - 3400), 0, 100);
}

bool charging() { return digitalRead(pins::CHARGE_STATUS) == LOW; }

bool buttonDown() { return digitalRead(pins::BOOT_BUTTON) == LOW; }

}  // namespace board
