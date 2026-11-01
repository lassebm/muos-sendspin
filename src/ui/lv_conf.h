/* LVGL configuration. Only what differs from LVGL's defaults
 * (lv_conf_internal.h fills in everything not defined here). LVGL v9.6. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888  /* uploaded straight into an SDL ARGB8888 texture */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB  /* plain malloc: no fixed LVGL heap to size */
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE         /* driven from our main loop only */
#define LV_DEF_REFR_PERIOD 33        /* at most ~30 fps; LVGL only redraws invalidated areas */

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_FONT_MONTSERRAT_14 1      /* default + symbol glyphs (play, pause, ...) as fallback */
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_USE_TINY_TTF 1            /* our Noto Sans, from memory */
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_USE_QRCODE 1              /* pairing: this player's token, and codes a server asks to show as QR */


#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
