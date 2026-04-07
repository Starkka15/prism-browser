#pragma once

#include <wpe/wpe-egl.h>
#include <EGL/egl.h>
#include <stdint.h>

/* ── Renderer backend (EGL display) ────────────────────────────────────────
 * One instance lives in the WebContent process.
 * fd: Unix socket end used to send frame-done signals to the UI process.
 */
struct sdl_backend {
    int fd;                 /* socket to view-backend (for frame sync) */
    EGLDisplay egl_display; /* cached after first get_native_display call */
};

/* ── Renderer target (one EGL surface per web view) ──────────────────────
 * Owns the EGL window surface that WebKit renders into.
 */
struct sdl_target {
    struct wpe_renderer_backend_egl_target *wpe_target;
    int fd;
    uint32_t width;
    uint32_t height;
};

/* ── Offscreen target (used for offscreen rendering / compositor) ─────── */
struct sdl_offscreen_target {
    /* nothing extra needed for our simple fullscreen setup */
    int dummy;
};

extern const struct wpe_renderer_backend_egl_interface            sdl_renderer_backend_egl_interface;
extern const struct wpe_renderer_backend_egl_target_interface     sdl_renderer_backend_egl_target_interface;
extern const struct wpe_renderer_backend_egl_offscreen_target_interface
                                                                   sdl_renderer_backend_egl_offscreen_target_interface;
