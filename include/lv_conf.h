/* LVGL configuration. Anything not set here keeps LVGL's default (lv_conf_internal.h). */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* Use the C library allocator: large buffers (GIF canvases) land in PSRAM automatically. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE /* all LVGL calls are serialized by ui::lock() */
#define LV_DEF_REFR_PERIOD 20
#define LV_USE_LOG 0

#define LV_USE_GIF 1

/* Font sizes used by layouts: sm 14, md 18, lg 28, xl 48 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1 /* status screens */
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_18

#endif
