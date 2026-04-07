#pragma once

#include <wpe/wpe.h>
#include <stdint.h>

#define WEBOS_SCREEN_W 1024
#define WEBOS_SCREEN_H 768

/* ── View backend (UI process side) ─────────────────────────────────────── */
struct sdl_view_backend {
    struct wpe_view_backend *wpe_backend;

    /* socketpair: host_fd kept here, renderer_fd sent to renderer process */
    int host_fd;
    int renderer_fd;

    /* Touch state: single active touch point */
    int touch_active;
    int32_t touch_x;
    int32_t touch_y;
};

extern const struct wpe_view_backend_interface sdl_view_backend_interface;
