/* lv_conf.h — minimal LVGL v9.2 config for the PC simulator.
 * Anything not set here falls back to lv_conf_internal.h defaults.
 * The firmware (Track 3) gets its own lv_conf.h sized for the ESP32-S3. */
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* PC build: use the C library directly */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_SDL 1

#define LV_USE_CANVAS 1
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
#endif
