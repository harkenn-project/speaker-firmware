// Spotpear ESP32-S3 1.28" BOX: pins and hardware access.
#pragma once
#include <Arduino.h>

namespace pins {
constexpr int LCD_SCLK = 4, LCD_MOSI = 2, LCD_CS = 5, LCD_DC = 47, LCD_RST = 38, LCD_BL = 42;  // BL is active-low
constexpr int TOUCH_SDA = 11, TOUCH_SCL = 7, TOUCH_RST = 6, TOUCH_INT = 12;
constexpr int CODEC_SDA = 15, CODEC_SCL = 14, AMP_EN = 46;
constexpr int I2S_MCLK = 16, I2S_WS = 45, I2S_BCLK = 9, I2S_DIN = 10, I2S_DOUT = 8;
constexpr int BOOT_BUTTON = 0, BATTERY_ADC = 1, CHARGE_STATUS = 41, LED = 48;
}  // namespace pins

namespace board {
constexpr int SCREEN_SIZE = 240;

// Initializes the panel, backlight, touch and LVGL (display + touch input).
void begin();
void setBrightness(int percent);
uint32_t lastTouchMs();
void markActivity();  // counts as a touch, e.g. for the button
int batteryPercent();
bool charging();
bool buttonDown();
}  // namespace board
