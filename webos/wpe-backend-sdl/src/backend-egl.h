#pragma once

#include <wpe/wpe-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stddef.h>

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

    /* Shared-memory framebuffer: written after each rendered frame */
    int    fb_fd;
    void  *fb_pixels;
    size_t fb_size;

    /*
     * EGL PBuffer surface used as the WebKit draw target.
     *
     * The PDK EGL window surface (NativeWindowType=0) is write-only — glReadPixels
     * on it returns GL_INVALID_VALUE.  In frame_will_render we redirect the current
     * EGL draw surface to pb_surf (a PBuffer).  WebKit renders into it via FBO 0.
     * In frame_rendered we read back via EGL lock surface (preferred) or
     * glReadPixels (fallback) into the shared-memory framebuffer.
     */
    EGLDisplay pb_dpy;
    EGLSurface pb_surf;
    EGLContext pb_ctx;    /* the WebKit EGL context, cached for eglMakeCurrent */
    EGLSurface win_surf;  /* original window surface, restored after readback */

    /*
     * EGL extension function pointers — loaded once in ensure_pbuffer.
     * NULL means the extension is unavailable on this driver.
     */
    PFNEGLCREATESYNCKHRPROC     pfn_CreateSync;
    PFNEGLCLIENTWAITSYNCKHRPROC pfn_ClientWaitSync;
    PFNEGLDESTROYSYNCKHRPROC    pfn_DestroySync;
    PFNEGLLOCKSURFACEKHRPROC    pfn_LockSurface;
    PFNEGLUNLOCKSURFACEKHRPROC  pfn_UnlockSurface;

    int has_lock;   /* 1 if PBuffer config supports EGL_KHR_lock_surface */
    int exts_loaded; /* 1 once extension pointers have been resolved */
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
