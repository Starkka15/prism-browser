/*
 * wpe-backend-sdl: EGL renderer backend for HP TouchPad (PDK / PowerVR)
 *
 * Lives in the WebContent process.  Responsible for:
 *   - Providing the EGL native display/window handles to WebKit
 *   - Signalling frame-done to the UI process over a Unix socket
 *
 * PDK EGL notes:
 *   - EGL_DEFAULT_DISPLAY is the only valid display on this platform
 *   - NativeWindowType = 0  →  fullscreen PowerVR surface
 *   - platform id = 0  →  use plain eglGetDisplay (not the platform extension)
 */

#include "backend-egl.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

/* ── Renderer backend ───────────────────────────────────────────────────── */

static void *
renderer_backend_create(int fd)
{
    struct sdl_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->fd = fd;
    b->egl_display = EGL_NO_DISPLAY;
    return b;
}

static void
renderer_backend_destroy(void *data)
{
    struct sdl_backend *b = data;
    if (!b) return;
    if (b->fd >= 0)
        close(b->fd);
    free(b);
}

static EGLNativeDisplayType
renderer_backend_get_native_display(void *data)
{
    (void)data;
    /*
     * PDK PowerVR EGL only supports the default display.
     * EGL_DEFAULT_DISPLAY is ((EGLNativeDisplayType)0).
     */
    return EGL_DEFAULT_DISPLAY;
}

static uint32_t
renderer_backend_get_platform(void *data)
{
    (void)data;
    /*
     * Return 0 to tell libwpe to call plain eglGetDisplay() rather than
     * eglGetPlatformDisplay().  PDK EGL does not support the platform ext.
     */
    return 0;
}

const struct wpe_renderer_backend_egl_interface sdl_renderer_backend_egl_interface = {
    .create             = renderer_backend_create,
    .destroy            = renderer_backend_destroy,
    .get_native_display = renderer_backend_get_native_display,
    .get_platform       = renderer_backend_get_platform,
    ._wpe_reserved1     = NULL,
    ._wpe_reserved2     = NULL,
    ._wpe_reserved3     = NULL,
};

/* ── Renderer target ────────────────────────────────────────────────────── */

static void *
renderer_target_create(struct wpe_renderer_backend_egl_target *wpe_target, int fd)
{
    struct sdl_target *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->wpe_target = wpe_target;
    t->fd         = fd;
    t->width      = 1024;
    t->height     = 768;
    return t;
}

static void
renderer_target_destroy(void *data)
{
    struct sdl_target *t = data;
    if (!t) return;
    /* fd is owned by the backend, not the target */
    free(t);
}

static void
renderer_target_initialize(void *data, void *backend_data,
                            uint32_t width, uint32_t height)
{
    struct sdl_target *t = data;
    (void)backend_data;
    t->width  = width  ? width  : 1024;
    t->height = height ? height : 768;
}

static EGLNativeWindowType
renderer_target_get_native_window(void *data)
{
    (void)data;
    /*
     * PDK fullscreen EGL surface: native window handle is 0.
     * PowerVR EGL interprets (NativeWindowType)0 as the physical screen.
     */
    return (EGLNativeWindowType)0;
}

static void
renderer_target_resize(void *data, uint32_t width, uint32_t height)
{
    struct sdl_target *t = data;
    t->width  = width;
    t->height = height;
}

static void
renderer_target_frame_will_render(void *data)
{
    (void)data;
    /* Nothing to do pre-frame on PDK */
}

static void
renderer_target_frame_rendered(void *data)
{
    struct sdl_target *t = data;

    /*
     * Tell libwpe that this target's frame is complete.
     * libwpe will forward this to the UI process.
     */
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->wpe_target);

    /*
     * Signal the view backend (UI process) over the socket so it can call
     * wpe_view_backend_dispatch_frame_displayed and kick the next frame.
     */
    if (t->fd >= 0) {
        const char sig = 'F';
        write(t->fd, &sig, 1);
    }
}

static void
renderer_target_deinitialize(void *data)
{
    (void)data;
}

const struct wpe_renderer_backend_egl_target_interface
sdl_renderer_backend_egl_target_interface = {
    .create           = renderer_target_create,
    .destroy          = renderer_target_destroy,
    .initialize       = renderer_target_initialize,
    .get_native_window = renderer_target_get_native_window,
    .resize           = renderer_target_resize,
    .frame_will_render = renderer_target_frame_will_render,
    .frame_rendered   = renderer_target_frame_rendered,
    .deinitialize     = renderer_target_deinitialize,
    ._wpe_reserved1   = NULL,
    ._wpe_reserved2   = NULL,
    ._wpe_reserved3   = NULL,
};

/* ── Offscreen target ───────────────────────────────────────────────────── */

static void *
offscreen_target_create(void)
{
    struct sdl_offscreen_target *t = calloc(1, sizeof(*t));
    return t;
}

static void
offscreen_target_destroy(void *data)
{
    free(data);
}

static void
offscreen_target_initialize(void *data, void *backend_data)
{
    (void)data;
    (void)backend_data;
}

static EGLNativeWindowType
offscreen_target_get_native_window(void *data)
{
    (void)data;
    return (EGLNativeWindowType)0;
}

const struct wpe_renderer_backend_egl_offscreen_target_interface
sdl_renderer_backend_egl_offscreen_target_interface = {
    .create            = offscreen_target_create,
    .destroy           = offscreen_target_destroy,
    .initialize        = offscreen_target_initialize,
    .get_native_window = offscreen_target_get_native_window,
    ._wpe_reserved0    = NULL,
    ._wpe_reserved1    = NULL,
    ._wpe_reserved2    = NULL,
    ._wpe_reserved3    = NULL,
};
