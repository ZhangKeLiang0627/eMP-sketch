#include "hal/sketch_lvgl_hal.hpp"

#include <lvgl.h>

#include <cstdio>
#include <cstdlib>

// 各后端驱动头（LVGL 9 的 lvgl.h 会按 LV_USE_* 条件包含，这里显式包含以保证可用）
#if LV_USE_SDL
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#elif LV_USE_LINUX_FBDEV
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif

#if LV_USE_EVDEV
#include "src/drivers/evdev/lv_evdev.h"
#endif

namespace sketch {
namespace {

const char* envOrDefault(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

#if LV_USE_SDL
float envFloatOrDefault(const char* name, float fallback)
{
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    char* end          = nullptr;
    const float parsed = std::strtof(value, &end);
    return end && end != value && parsed > 0.0f ? parsed : fallback;
}
#endif

}  // namespace

bool initLvglHal(int32_t width, int32_t height)
{
    lv_group_set_default(lv_group_create());

#if LV_USE_SDL
    (void)width;
    (void)height;
    lv_display_t* display = lv_sdl_window_create(width, height);
    if (!display) {
        std::fprintf(stderr, "[sketch] failed to create SDL display\n");
        return false;
    }

    const float zoom = envFloatOrDefault("SKETCH_SDL_ZOOM", 1.0f);
    lv_sdl_window_set_resizeable(display, false);
    lv_sdl_window_set_zoom(display, zoom);
    lv_sdl_window_set_title(display, envOrDefault("LV_SDL_WINDOW_TITLE", "eMP-sketch"));
    std::fprintf(stderr, "[sketch] SDL display %dx%d, zoom %.2f\n", width, height, zoom);

    lv_indev_t* mouse = lv_sdl_mouse_create();
    if (mouse) {
        lv_indev_set_group(mouse, lv_group_get_default());
        lv_indev_set_display(mouse, display);
    }

    lv_indev_t* keyboard = lv_sdl_keyboard_create();
    if (keyboard) {
        lv_indev_set_group(keyboard, lv_group_get_default());
        lv_indev_set_display(keyboard, display);
    }
    return true;

#elif LV_USE_LINUX_FBDEV
    (void)width;
    (void)height;
    lv_display_t* display = lv_linux_fbdev_create();
    if (!display) {
        std::fprintf(stderr, "[sketch] failed to create framebuffer display\n");
        return false;
    }

    const char* fb_device = envOrDefault("SKETCH_FBDEV_DEVICE", "/dev/fb0");
    if (lv_linux_fbdev_set_file(display, fb_device) != LV_RESULT_OK) {
        std::fprintf(stderr, "[sketch] failed to open framebuffer %s\n", fb_device);
        return false;
    }
    std::fprintf(stderr, "[sketch] framebuffer display via %s\n", fb_device);

#if LV_USE_EVDEV
    const char* touch_path = envOrDefault("SKETCH_EVDEV_TOUCH", SKETCH_EVDEV_TOUCH);
    lv_indev_t* touch      = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    if (touch) {
        lv_indev_set_display(touch, display);
        std::fprintf(stderr, "[sketch] evdev touch via %s\n", touch_path);
    } else {
        std::fprintf(stderr, "[sketch] evdev touch unavailable at %s\n", touch_path);
    }
#endif  // LV_USE_EVDEV

    return true;

#else
    std::fprintf(stderr, "[sketch] no LVGL display driver enabled\n");
    return false;
#endif
}

void shutdownLvglHal()
{
#if LV_USE_SDL
    lv_sdl_quit();
#endif
}

}  // namespace sketch
