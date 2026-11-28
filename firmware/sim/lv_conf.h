/**
 * @file lv_conf.h
 * LVGL 9.5 desktop simulator configuration matching StackChan firmware sdkconfig.defaults.
 */
#if 1

#ifndef LV_CONF_H
#define LV_CONF_H

/* Color depth matching CoreS3 RGB565 display */
#define LV_COLOR_DEPTH 16

/* Standard C library allocators / string / sprintf (matching CONFIG_LV_USE_CLIB_*) */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD      16      /* ~60 FPS desktop refresh */
#define LV_DPI_DEF              130

#define LV_USE_OS               LV_OS_NONE

/* Logging */
#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF           1

/* Asserts */
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1
#define LV_USE_ASSERT_STYLE     1

/* Fonts matching firmware/sdkconfig.defaults */
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_FMT_TXT_LARGE   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* Core widgets enabled in firmware */
#define LV_USE_ARC              1
#define LV_USE_BAR              1
#define LV_USE_BUTTON           1
#define LV_USE_BUTTONMATRIX     1
#define LV_USE_CANVAS           1
#define LV_USE_CHECKBOX         1
#define LV_USE_DROPDOWN         1
#define LV_USE_IMAGE            1
#define LV_USE_IMAGEBUTTON      1
#define LV_USE_LABEL            1
#define LV_USE_LINE             1
#define LV_USE_ROLLER           1
#define LV_USE_SCALE            1
#define LV_USE_SLIDER           1
#define LV_USE_SPINNER          1
#define LV_USE_SWITCH           1
#define LV_USE_TEXTAREA         1

/* Disabled widgets matching sdkconfig.defaults */
#define LV_USE_ANIMIMG          0
#define LV_USE_KEYBOARD         0
#define LV_USE_LED              0
#define LV_USE_LIST             0
#define LV_USE_MENU             0
#define LV_USE_MSGBOX           0
#define LV_USE_SPAN             0
#define LV_USE_SPINBOX          0
#define LV_USE_TABVIEW          0
#define LV_USE_TILEVIEW         0
#define LV_USE_WIN              0

/* Extra libraries matching sdkconfig.defaults */
#define LV_USE_LODEPNG          1
#define LV_USE_QRCODE           1
#define LV_USE_SNAPSHOT         1
#define LV_USE_IMGFONT          1

/* SDL2 desktop driver */
#define LV_USE_SDL              1
#define LV_SDL_INCLUDE_PATH     <SDL.h>
#define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_SDL_BUF_COUNT        1
#define LV_SDL_FULLSCREEN       0
#define LV_SDL_DIRECT_EXIT      1
#define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER

#define LV_BUILD_EXAMPLES       0
#define LV_BUILD_DEMOS          0

#endif /* LV_CONF_H */
#endif
