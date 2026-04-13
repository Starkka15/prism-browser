/*
 * wpe-backend-sdl: EGL renderer backend for HP TouchPad (PDK / PowerVR)
 *
 * Lives in the WebContent process.  Responsible for:
 *   - Providing the EGL native display/window handles to WebKit
 *   - Capturing rendered frames into /tmp/prism-fb for the UI compositor
 *   - Signalling frame-done to the UI process over a Unix socket
 *
 * PDK EGL notes:
 *   - EGL_DEFAULT_DISPLAY is the only valid display on this platform
 *   - NativeWindowType = 0  →  fullscreen PowerVR surface
 *   - The EGL window surface (NativeWindowType=0) is write-only:
 *     glReadPixels returns GL_INVALID_VALUE on it.
 *
 * Pixel capture strategy (PBuffer redirect):
 *   1. In frame_will_render, switch the current EGL draw surface to a
 *      PBuffer we own.  WebKit's rendering targets FBO 0 = the PBuffer
 *      (readable) instead of the write-only window surface.
 *   2. WPE calls eglSwapBuffers(display, window_surf) — that's fine; the
 *      window surface may present blank but we don't use it for display.
 *   3. In frame_rendered:
 *      Fast path (EGL_KHR_fence_sync + EGL_KHR_lock_surface):
 *        - Insert GPU fence, wait for it (no implicit GL pipeline stall).
 *        - eglLockSurfaceKHR → direct CPU pointer to PBuffer pixels.
 *        - One memcpy into /tmp/prism-fb shm.  No GL state machine involved.
 *        - eglUnlockSurfaceKHR.
 *      Fallback: glReadPixels into /tmp/prism-fb (stalls GPU pipeline).
 *   4. Restore window surface as current, signal the UI process.
 */

#include "backend-egl.h"
#include "view-backend.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

/* ── Renderer backend ───────────────────────────────────────────────────── */

static void *
renderer_backend_create(int fd)
{
    fprintf(stderr, "[wpe-backend-sdl] renderer_backend_create fd=%d\n", fd);
    /*
     * Do NOT call SDL_Init(SDL_INIT_VIDEO) here.  WPEWebProcess is a separate
     * process; on webOS PDK, SDL_Init(SDL_INIT_VIDEO) registers a second
     * display connection with SysMgr.  When launched from the webOS launcher
     * (SysMgr actively managing display ownership), SysMgr hands the display
     * to WPEWebProcess and drops the main UI process — causing a black screen.
     * WPEWebProcess only needs EGL, which it obtains via eglGetDisplay(
     * EGL_DEFAULT_DISPLAY) directly through WebKit's internal EGL init.
     */

    struct sdl_backend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fd = fd;
    b->egl_display = EGL_NO_DISPLAY;
    return b;
}

static void
renderer_backend_destroy(void *data)
{
    struct sdl_backend *b = data;
    if (!b) return;
    if (b->fd >= 0) close(b->fd);
    free(b);
}

static EGLNativeDisplayType
renderer_backend_get_native_display(void *data)
{
    (void)data;
    fprintf(stderr, "[wpe-backend-sdl] get_native_display → EGL_DEFAULT_DISPLAY\n");
    return EGL_DEFAULT_DISPLAY;
}

static uint32_t
renderer_backend_get_platform(void *data)
{
    (void)data;
    return 0; /* plain eglGetDisplay, not the platform extension */
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
    if (!t) return NULL;
    t->wpe_target = wpe_target;
    t->fd         = fd;
    t->width      = WEBOS_SCREEN_W;
    t->height     = WEBKIT_H;   /* 704 — chrome bar excluded */
    t->pb_dpy     = EGL_NO_DISPLAY;
    t->pb_surf    = EGL_NO_SURFACE;
    t->pb_ctx     = EGL_NO_CONTEXT;
    t->win_surf   = EGL_NO_SURFACE;
    return t;
}

static void
renderer_target_destroy(void *data)
{
    struct sdl_target *t = data;
    if (!t) return;
    if (t->pb_surf != EGL_NO_SURFACE && t->pb_dpy != EGL_NO_DISPLAY)
        eglDestroySurface(t->pb_dpy, t->pb_surf);
    if (t->fb_pixels)
        munmap(t->fb_pixels, t->fb_size);
    if (t->fb_fd >= 0)
        close(t->fb_fd);
    free(t);
}

static void
renderer_target_initialize(void *data, void *backend_data,
                            uint32_t width, uint32_t height)
{
    struct sdl_target *t = data;
    (void)backend_data;
    t->width  = width  ? width  : WEBOS_SCREEN_W;
    t->height = height ? height : WEBOS_SCREEN_H;
    fprintf(stderr, "[wpe-backend-sdl] renderer_target_initialize %ux%u\n",
            t->width, t->height);

    /*
     * Create and map /tmp/prism-fb.  Written in frame_rendered via
     * glReadPixels from the PBuffer; read by the UI process compositor.
     */
    t->fb_size   = (size_t)t->width * t->height * 4; /* RGBA8 */
    t->fb_fd     = open(PRISM_FB_PATH, O_CREAT | O_RDWR, 0600);
    if (t->fb_fd < 0) {
        perror("[wpe-backend-sdl] open prism-fb");
    } else {
        if (ftruncate(t->fb_fd, (off_t)t->fb_size) < 0)
            perror("[wpe-backend-sdl] ftruncate prism-fb");
        t->fb_pixels = mmap(NULL, t->fb_size,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            t->fb_fd, 0);
        if (t->fb_pixels == MAP_FAILED) {
            perror("[wpe-backend-sdl] mmap prism-fb");
            t->fb_pixels = NULL;
        }
        fprintf(stderr, "[wpe-backend-sdl] prism-fb mapped: %zu bytes at %p\n",
                t->fb_size, t->fb_pixels);
    }
    /* PBuffer is created lazily in frame_will_render once context is current. */
}

static EGLNativeWindowType
renderer_target_get_native_window(void *data)
{
    (void)data;
    return (EGLNativeWindowType)0; /* PDK fullscreen surface */
}

static void
renderer_target_resize(void *data, uint32_t width, uint32_t height)
{
    struct sdl_target *t = data;
    t->width  = width;
    t->height = height;
}

/* ── EGL extension loading ───────────────────────────────────────────────── */

static void
load_egl_extensions(struct sdl_target *t)
{
    if (t->exts_loaded) return;
    t->exts_loaded = 1;

    t->pfn_CreateSync = (PFNEGLCREATESYNCKHRPROC)
        eglGetProcAddress("eglCreateSyncKHR");
    t->pfn_ClientWaitSync = (PFNEGLCLIENTWAITSYNCKHRPROC)
        eglGetProcAddress("eglClientWaitSyncKHR");
    t->pfn_DestroySync = (PFNEGLDESTROYSYNCKHRPROC)
        eglGetProcAddress("eglDestroySyncKHR");
    t->pfn_LockSurface = (PFNEGLLOCKSURFACEKHRPROC)
        eglGetProcAddress("eglLockSurfaceKHR");
    t->pfn_UnlockSurface = (PFNEGLUNLOCKSURFACEKHRPROC)
        eglGetProcAddress("eglUnlockSurfaceKHR");

    fprintf(stderr,
            "[wpe-backend-sdl] EGL exts: FenceSync=%s LockSurface=%s\n",
            (t->pfn_CreateSync && t->pfn_ClientWaitSync && t->pfn_DestroySync)
                ? "YES" : "NO",
            (t->pfn_LockSurface && t->pfn_UnlockSurface) ? "YES" : "NO");
}

/* ── PBuffer creation (called once, when GL context is first current) ────── */

static void
ensure_pbuffer(struct sdl_target *t)
{
    if (t->pb_surf != EGL_NO_SURFACE)
        return; /* already created */

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLSurface win = eglGetCurrentSurface(EGL_DRAW);

    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[wpe-backend-sdl] ensure_pbuffer: no EGL context\n");
        return;
    }

    t->pb_dpy   = dpy;
    t->pb_ctx   = ctx;
    t->win_surf = win;

    /* Load EGL extension function pointers now that we have a display */
    load_egl_extensions(t);

    /* Dump EGL info */
    fprintf(stderr, "[wpe-backend-sdl] EGL_VERSION:    %s\n",
            eglQueryString(dpy, EGL_VERSION));
    fprintf(stderr, "[wpe-backend-sdl] EGL_VENDOR:     %s\n",
            eglQueryString(dpy, EGL_VENDOR));
    fprintf(stderr, "[wpe-backend-sdl] EGL_EXTENSIONS: %s\n",
            eglQueryString(dpy, EGL_EXTENSIONS));

    /* Find the config used for the existing context */
    EGLint config_id = 0;
    eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &config_id);
    fprintf(stderr, "[wpe-backend-sdl] context config_id=%d\n", config_id);

    EGLConfig configs[64];
    EGLint    n_configs = 0;
    eglGetConfigs(dpy, configs, 64, &n_configs);

    EGLConfig pb_config = NULL;
    for (EGLint i = 0; i < n_configs; i++) {
        EGLint id = 0, surf_type = 0;
        eglGetConfigAttrib(dpy, configs[i], EGL_CONFIG_ID,    &id);
        eglGetConfigAttrib(dpy, configs[i], EGL_SURFACE_TYPE, &surf_type);
        if (id == config_id) {
            if (surf_type & EGL_PBUFFER_BIT) {
                pb_config = configs[i];
                fprintf(stderr,
                        "[wpe-backend-sdl] config %d PBUFFER=YES LOCK=%s"
                        " (surf_type=0x%x)\n",
                        id,
                        (surf_type & EGL_LOCK_SURFACE_BIT_KHR) ? "YES" : "NO",
                        surf_type);
                if (surf_type & EGL_LOCK_SURFACE_BIT_KHR)
                    t->has_lock = 1;
            } else {
                fprintf(stderr,
                        "[wpe-backend-sdl] config %d NO PBUFFER bit"
                        " (surf_type=0x%x)\n", id, surf_type);
            }
            break;
        }
    }

    if (!pb_config) {
        /*
         * The context's config doesn't advertise EGL_PBUFFER_BIT.  Fall back
         * to scanning for a compatible config, or try with the same config
         * anyway — some PDK drivers omit the flag but still support it.
         */
        fprintf(stderr,
                "[wpe-backend-sdl] context config has no PBUFFER_BIT —"
                " trying with same config anyway\n");
        for (EGLint i = 0; i < n_configs; i++) {
            EGLint id = 0;
            eglGetConfigAttrib(dpy, configs[i], EGL_CONFIG_ID, &id);
            if (id == config_id) { pb_config = configs[i]; break; }
        }
    }

    if (!pb_config) {
        fprintf(stderr, "[wpe-backend-sdl] ensure_pbuffer: config not found\n");
        return;
    }

    const EGLint pb_attrs[] = {
        EGL_WIDTH,  (EGLint)t->width,
        EGL_HEIGHT, (EGLint)t->height,
        EGL_NONE
    };
    t->pb_surf = eglCreatePbufferSurface(dpy, pb_config, pb_attrs);
    if (t->pb_surf == EGL_NO_SURFACE) {
        fprintf(stderr,
                "[wpe-backend-sdl] eglCreatePbufferSurface failed: 0x%x\n",
                (unsigned)eglGetError());
        return;
    }
    fprintf(stderr,
            "[wpe-backend-sdl] PBuffer surface created: %p (%ux%u) has_lock=%d\n",
            (void *)t->pb_surf, t->width, t->height, t->has_lock);
}

/* ── frame_will_render ───────────────────────────────────────────────────── */

static void
renderer_target_frame_will_render(void *data)
{
    struct sdl_target *t = data;

    EGLContext ctx = eglGetCurrentContext();
    EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    fprintf(stderr, "[wpe-backend-sdl] frame_will_render: ctx=%p surf=%p\n",
            (void *)ctx, (void *)surf);

    /* Create PBuffer on first frame when EGL context is current */
    ensure_pbuffer(t);

    if (t->pb_surf == EGL_NO_SURFACE)
        return;

    /*
     * Redirect the EGL draw surface from the write-only window surface to
     * our PBuffer.  WebKit will render into FBO 0 which now maps to the
     * PBuffer — which is readable via glReadPixels.
     *
     * WPE will still call eglSwapBuffers(display, window_surf); that call
     * operates on the original window surface (not the current draw surface),
     * so it may produce an EGL_BAD_SURFACE error or simply present a blank
     * frame.  Either way we don't use the window surface for display.
     */
    EGLBoolean ok = eglMakeCurrent(t->pb_dpy, t->pb_surf, t->pb_surf, t->pb_ctx);
    fprintf(stderr,
            "[wpe-backend-sdl] switched to PBuffer draw surface: %s (egl=0x%x)\n",
            ok ? "OK" : "FAIL", ok ? 0 : (unsigned)eglGetError());
}

/* ── frame_rendered ─────────────────────────────────────────────────────── */

/*
 * Pixel readback strategy:
 *
 * Fast path (EGL_KHR_lock_surface available on PBuffer config):
 *   1. Insert an EGL fence sync — tells the GPU to signal when it finishes
 *      writing the PBuffer.  eglClientWaitSyncKHR blocks until the signal
 *      arrives, so we know the PBuffer pixels are stable before we touch them.
 *   2. eglLockSurfaceKHR — the driver maps the PBuffer's colour buffer directly
 *      into CPU address space.  No GL pipeline involvement, no format
 *      conversion, no redundant copy.
 *   3. One memcpy (or row-by-row if there is stride padding) into the shm.
 *   4. eglUnlockSurfaceKHR.
 *
 * Fallback path (glReadPixels):
 *   Used when EGL_LOCK_SURFACE_BIT_KHR was not set on the PBuffer config, or
 *   when the lock surface extension functions are unavailable.  glReadPixels
 *   implicitly stalls the GPU pipeline and triggers an internal format
 *   conversion — kept purely as a safety net.
 */
static void
renderer_target_frame_rendered(void *data)
{
    struct sdl_target *t = data;

    if (t->pb_surf == EGL_NO_SURFACE || !t->fb_pixels)
        goto signal;

    /* Ensure PBuffer is the current draw/read surface */
    if (eglGetCurrentSurface(EGL_DRAW) != t->pb_surf)
        eglMakeCurrent(t->pb_dpy, t->pb_surf, t->pb_surf, t->pb_ctx);

    /* ── Step 1: GPU fence — wait for rendering into PBuffer to finish ── */
    if (t->pfn_CreateSync && t->pfn_ClientWaitSync && t->pfn_DestroySync) {
        EGLSyncKHR fence = t->pfn_CreateSync(t->pb_dpy, EGL_SYNC_FENCE_KHR, NULL);
        if (fence != EGL_NO_SYNC_KHR) {
            t->pfn_ClientWaitSync(t->pb_dpy, fence,
                                  EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                  EGL_FOREVER_KHR);
            t->pfn_DestroySync(t->pb_dpy, fence);
        }
    }

    /* Drain any GL errors WebKit left behind before we touch the surface */
    { GLenum e; while ((e = glGetError()) != GL_NO_ERROR)
          fprintf(stderr, "[wpe-backend-sdl] GL err drained: 0x%x\n", e); }

    if (t->has_lock && t->pfn_LockSurface && t->pfn_UnlockSurface) {
        /* ── Fast path: EGL lock surface ── */
        static const EGLint lock_attrs[] = {
            EGL_LOCK_USAGE_HINT_KHR, EGL_READ_SURFACE_BIT_KHR,
            EGL_NONE
        };
        if (t->pfn_LockSurface(t->pb_dpy, t->pb_surf, lock_attrs)) {
            EGLint bitmap_ptr_raw = 0, bitmap_pitch = 0;
            EGLint origin = EGL_UPPER_LEFT_KHR; /* safe default */
            eglQuerySurface(t->pb_dpy, t->pb_surf,
                            EGL_BITMAP_POINTER_KHR, &bitmap_ptr_raw);
            eglQuerySurface(t->pb_dpy, t->pb_surf,
                            EGL_BITMAP_PITCH_KHR,   &bitmap_pitch);
            eglQuerySurface(t->pb_dpy, t->pb_surf,
                            EGL_BITMAP_ORIGIN_KHR,  &origin);

            const uint8_t *src = (const uint8_t *)(uintptr_t)(uint32_t)bitmap_ptr_raw;
            uint8_t       *dst = (uint8_t *)t->fb_pixels;
            size_t row_bytes   = (size_t)t->width * 4;

            if (src) {
                if (origin == EGL_UPPER_LEFT_KHR) {
                    /*
                     * Upper-left origin: bitmap row 0 = top of surface.
                     * glReadPixels convention is lower-left (row 0 = bottom).
                     * Flip rows so the shm layout matches what glReadPixels
                     * would have produced — the UI compositor expects that.
                     */
                    const uint8_t *s = src + (size_t)(t->height - 1) * bitmap_pitch;
                    for (uint32_t r = 0; r < t->height;
                         r++, s -= bitmap_pitch, dst += row_bytes)
                        memcpy(dst, s, row_bytes);
                } else {
                    /*
                     * EGL_LOWER_LEFT_KHR: rows are in the same bottom-up
                     * order as glReadPixels — straight copy.
                     */
                    if ((size_t)bitmap_pitch == row_bytes) {
                        memcpy(dst, src, t->fb_size);
                    } else {
                        const uint8_t *s = src;
                        for (uint32_t r = 0; r < t->height;
                             r++, s += bitmap_pitch, dst += row_bytes)
                            memcpy(dst, s, row_bytes);
                    }
                }

                /* First-frame diagnostic */
                static int lock_logged = 0;
                if (!lock_logged) {
                    lock_logged = 1;
                    const uint8_t *px = (const uint8_t *)t->fb_pixels
                        + ((t->height/2) * t->width + (t->width/2)) * 4;
                    fprintf(stderr,
                            "[wpe-backend-sdl] lock readback OK:"
                            " origin=%s pitch=%d center=(%d,%d,%d,%d)\n",
                            origin == EGL_LOWER_LEFT_KHR ? "lower-left" : "upper-left",
                            bitmap_pitch, px[0], px[1], px[2], px[3]);
                }
            }

            t->pfn_UnlockSurface(t->pb_dpy, t->pb_surf);
        } else {
            fprintf(stderr,
                    "[wpe-backend-sdl] eglLockSurface failed (0x%x),"
                    " falling back to glReadPixels\n",
                    (unsigned)eglGetError());
            t->has_lock = 0; /* don't try again */
            goto readpixels;
        }
    } else {
readpixels:
        /* ── Fallback: glReadPixels (stalls GPU pipeline) ── */
        glReadPixels(0, 0, (GLsizei)t->width, (GLsizei)t->height,
                     GL_RGBA, GL_UNSIGNED_BYTE, t->fb_pixels);
        GLenum err = glGetError();
        static int rp_logged = 0;
        if (!rp_logged) {
            rp_logged = 1;
            const uint8_t *px = (const uint8_t *)t->fb_pixels
                + ((t->height/2) * t->width + (t->width/2)) * 4;
            fprintf(stderr,
                    "[wpe-backend-sdl] glReadPixels fallback:"
                    " center=(%d,%d,%d,%d) err=0x%x\n",
                    px[0], px[1], px[2], px[3], (unsigned)err);
        }
    }

    /* Restore window surface so WPE's next eglSwapBuffers works */
    if (t->win_surf != EGL_NO_SURFACE)
        eglMakeCurrent(t->pb_dpy, t->win_surf, t->win_surf, t->pb_ctx);

signal:
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->wpe_target);

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
    .create            = renderer_target_create,
    .destroy           = renderer_target_destroy,
    .initialize        = renderer_target_initialize,
    .get_native_window = renderer_target_get_native_window,
    .resize            = renderer_target_resize,
    .frame_will_render = renderer_target_frame_will_render,
    .frame_rendered    = renderer_target_frame_rendered,
    .deinitialize      = renderer_target_deinitialize,
    ._wpe_reserved1    = NULL,
    ._wpe_reserved2    = NULL,
    ._wpe_reserved3    = NULL,
};

/* ── Offscreen target ───────────────────────────────────────────────────── */

static void *
offscreen_target_create(void)
{
    return calloc(1, sizeof(struct sdl_offscreen_target));
}

static void
offscreen_target_destroy(void *data) { free(data); }

static void
offscreen_target_initialize(void *data, void *backend_data)
{
    (void)data; (void)backend_data;
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
