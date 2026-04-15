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
#include <errno.h>
#include <sys/mman.h>
#include <sys/socket.h>   /* recv, MSG_DONTWAIT */
#include <sys/stat.h>     /* mkfifo */

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
    t->wpe_target     = wpe_target;
    t->fd             = fd;
    t->width          = 1024;   /* default; overridden by renderer_target_initialize */
    t->height         = 704;    /* default; overridden by renderer_target_initialize */
    t->pb_dpy         = EGL_NO_DISPLAY;
    t->pb_surf        = EGL_NO_SURFACE;
    t->pb_ctx         = EGL_NO_CONTEXT;
    t->win_surf       = EGL_NO_SURFACE;
    t->frame_fifo_fd  = -1;
    return t;
}

static void
renderer_target_destroy(void *data)
{
    struct sdl_target *t = data;
    if (!t) return;
    if (t->qcom_created_image != EGL_NO_IMAGE_KHR
            && t->pfn_DestroyImage && t->pb_dpy != EGL_NO_DISPLAY)
        t->pfn_DestroyImage(t->pb_dpy, t->qcom_created_image);
    if (t->pb_surf != EGL_NO_SURFACE && t->pb_dpy != EGL_NO_DISPLAY)
        eglDestroySurface(t->pb_dpy, t->pb_surf);
    if (t->fb_pixels)
        munmap(t->fb_pixels, t->fb_size);
    if (t->fb_fd >= 0)
        close(t->fb_fd);
    if (t->frame_fifo_fd >= 0)
        close(t->frame_fifo_fd);
    free(t);
}

static void
renderer_target_initialize(void *data, void *backend_data,
                            uint32_t width, uint32_t height)
{
    struct sdl_target *t = data;
    (void)backend_data;
    t->width  = width  ? width  : 1024;
    t->height = height ? height : 704;
    fprintf(stderr, "[wpe-backend-sdl] renderer_target_initialize %ux%u\n",
            t->width, t->height);

    /*
     * Receive 8-byte sync packet from UI (content ignored).
     * Use MSG_DONTWAIT: for the first WebProcess the UI sends {0,0} and we
     * receive it; for subsequent WebProcesses WPE may reuse this target or
     * the fd may not have data yet — proceed regardless rather than blocking
     * indefinitely or failing with EBADF when the fd is recycled.
     */
    {
        EGLint pkt[2] = { 0, 0 };
        ssize_t n = recv(t->fd, pkt, sizeof(pkt), MSG_DONTWAIT);
        fprintf(stderr,
                "[wpe-backend-sdl] sync recv from UI: n=%zd (errno=%d)"
                " — WP will create QCOM image in ensure_pbuffer\n", n, errno);
    }

    /* Always set up prism-fb as fallback.  Used if QCOM creation/setup fails
     * in ensure_pbuffer.  UI also creates prism-fb unconditionally now, so it
     * should already exist when we open it; create if absent. */
    t->fb_size = (size_t)t->width * t->height * 4;
    t->fb_fd   = open(PRISM_FB_PATH, O_RDWR);
    if (t->fb_fd < 0) {
        t->fb_fd = open(PRISM_FB_PATH, O_CREAT | O_RDWR, 0666);
        if (t->fb_fd >= 0)
            ftruncate(t->fb_fd, (off_t)t->fb_size);
    }
    if (t->fb_fd >= 0) {
        t->fb_pixels = mmap(NULL, t->fb_size,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            t->fb_fd, 0);
        if (t->fb_pixels == MAP_FAILED) {
            perror("[wpe-backend-sdl] mmap prism-fb");
            t->fb_pixels = NULL;
        } else {
            fprintf(stderr,
                    "[wpe-backend-sdl] prism-fb mapped: %zu bytes (fallback)\n",
                    t->fb_size);
        }
    } else {
        perror("[wpe-backend-sdl] open prism-fb");
    }
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

    /* QCOM zero-copy: EGLImage import + GL_OES_EGL_image */
    t->pfn_CreateImage = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    t->pfn_DestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    t->pfn_ImageTargetTex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");

    fprintf(stderr,
            "[wpe-backend-sdl] EGL exts: FenceSync=%s LockSurface=%s"
            " EGLImage=%s GL_OES_EGL_image=%s\n",
            (t->pfn_CreateSync && t->pfn_ClientWaitSync && t->pfn_DestroySync)
                ? "YES" : "NO",
            (t->pfn_LockSurface && t->pfn_UnlockSurface) ? "YES" : "NO",
            (t->pfn_CreateImage && t->pfn_DestroyImage) ? "YES" : "NO",
            t->pfn_ImageTargetTex ? "YES" : "NO");
}

/* ── PBuffer creation (called once, when GL context is first current) ────── */

static void
ensure_pbuffer(struct sdl_target *t)
{
    if (t->pb_surf != EGL_NO_SURFACE) {
        /*
         * WPE may reuse this sdl_target struct across WebProcess respawns
         * without calling renderer_target_destroy/create.  EGL handles from
         * the dead process are meaningless in the new process — reset everything
         * so we re-create the PBuffer below.
         */
        if (t->init_pid != (int)getpid()) {
            fprintf(stderr,
                    "[wpe-backend-sdl] ensure_pbuffer: new pid=%d (was %d)"
                    " — resetting stale EGL state\n",
                    (int)getpid(), t->init_pid);
            /* Don't call eglDestroySurface — handle is invalid in this process */
            t->pb_surf     = EGL_NO_SURFACE;
            t->pb_dpy      = EGL_NO_DISPLAY;
            t->pb_ctx      = EGL_NO_CONTEXT;
            t->win_surf    = EGL_NO_SURFACE;
            t->exts_loaded = 0;
            t->has_lock    = 0;
            /* Reset QCOM state — image handle invalid in new process */
            t->qcom_ready        = 0;
            t->qcom_ack_pending  = 0;
            t->qcom_tex          = 0;
            t->qcom_share_id     = 0;
            t->qcom_created_image = EGL_NO_IMAGE_KHR;
            t->pfn_CreateSharedImage_wp = NULL;
            t->pfn_CreateImage   = NULL;
            t->pfn_DestroyImage  = NULL;
            t->pfn_ImageTargetTex = NULL;
            t->pfn_CreateSync    = NULL;
            t->pfn_ClientWaitSync = NULL;
            t->pfn_DestroySync   = NULL;
            t->pfn_LockSurface   = NULL;
            t->pfn_UnlockSurface = NULL;
            /* Close stale FIFO write end — will re-open below */
            if (t->frame_fifo_fd >= 0) {
                close(t->frame_fifo_fd);
                t->frame_fifo_fd = -1;
            }
            /* init_pid will be set after successful PBuffer creation below */
        } else {
            return; /* already created in this process */
        }
    }

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

    /* ── WP creates QCOM shared image (reversed from UI-creates approach) ───
     * By creating on WP's own display (pb_dpy) with its real GL context,
     * we avoid the cross-process EGL handle mismatch: UI's sdl_egl_dpy
     * (e.g. 0x2) is not a valid display in WP's process space, so import
     * using it returned EGL_BAD_DISPLAY.  Creating here, then having UI
     * import on its own valid display (0x2), should succeed.
     *
     * Must be done AFTER load_egl_extensions so pfn_* are resolved.
     * Sends {share_id, dpy} packet to UI (over t->fd) so UI can import.
     * If creation fails, sends {0, 0} → UI stays on prism-fb fallback.
     */
    t->pfn_CreateSharedImage_wp = (PFNEGLCREATESHAREDIMAGEQUALCOMMPROC)
        eglGetProcAddress("eglCreateSharedImageQUALCOMM");
    {
        int qcom_ok = 0;
        if (t->pfn_CreateSharedImage_wp && t->pfn_ImageTargetTex
                && t->pfn_DestroyImage) {
            const EGLint sz_attribs[] = {
                EGL_HEIGHT, (EGLint)t->height,
                EGL_WIDTH,  (EGLint)t->width,
                EGL_NONE
            };
            eglGetError(); /* clear */
            EGLImageKHR img = t->pfn_CreateSharedImage_wp(
                t->pb_dpy, t->pb_ctx, NULL, sz_attribs);
            fprintf(stderr,
                    "[wpe-backend-sdl] eglCreateSharedImageQUALCOMM(WP)"
                    " -> %p (egl_err=0x%x)\n",
                    (void *)img, (unsigned)eglGetError());
            if (img != EGL_NO_IMAGE_KHR) {
                t->qcom_created_image = img;
                t->qcom_share_id = (EGLint)(uintptr_t)img;
                glGenTextures(1, &t->qcom_tex);
                glBindTexture(GL_TEXTURE_2D, t->qcom_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glGetError(); /* clear any pending GL error */
                t->pfn_ImageTargetTex(GL_TEXTURE_2D, (GLeglImageOES)img);
                GLenum gl_err = glGetError();
                if (gl_err == GL_NO_ERROR) {
                    /* Don't set qcom_ready yet — wait for UI to confirm it
                     * successfully imported the image (ACK in frame_rendered).
                     * WP uses prism-fb until ACK=1 arrives. */
                    qcom_ok = 1;
                    t->qcom_ack_pending = 1;
                    fprintf(stderr,
                            "[wpe-backend-sdl] WP QCOM tex ready:"
                            " share_id=0x%x tex=%u — awaiting UI ACK\n",
                            (unsigned)t->qcom_share_id, t->qcom_tex);
                } else {
                    fprintf(stderr,
                            "[wpe-backend-sdl] glEGLImageTargetTexture2DOES"
                            " on created image failed: 0x%x\n",
                            (unsigned)gl_err);
                    glDeleteTextures(1, &t->qcom_tex);
                    t->qcom_tex = 0;
                    t->pfn_DestroyImage(t->pb_dpy, img);
                    t->qcom_created_image = EGL_NO_IMAGE_KHR;
                    t->qcom_share_id = 0;
                }
            } else {
                fprintf(stderr,
                        "[wpe-backend-sdl] eglCreateSharedImageQUALCOMM"
                        " failed\n");
            }
        } else {
            fprintf(stderr,
                    "[wpe-backend-sdl] QCOM exts not available:"
                    " CreateSharedImage=%s ImageTargetTex=%s\n",
                    t->pfn_CreateSharedImage_wp ? "YES" : "NO",
                    t->pfn_ImageTargetTex       ? "YES" : "NO");
        }
        /* Inform UI: {share_id, dpy} or {0, 0} */
        EGLint pkt[2] = {
            qcom_ok ? t->qcom_share_id : 0,
            qcom_ok ? (EGLint)(uintptr_t)t->pb_dpy : 0
        };
        ssize_t sent = write(t->fd, pkt, sizeof(pkt));
        fprintf(stderr,
                "[wpe-backend-sdl] QCOM init sent to UI:"
                " share_id=0x%x dpy=0x%x (%zd bytes)\n",
                (unsigned)pkt[0], (unsigned)pkt[1], sent);
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
    t->init_pid = (int)getpid();
    fprintf(stderr,
            "[wpe-backend-sdl] PBuffer surface created: %p (%ux%u) has_lock=%d pid=%d\n",
            (void *)t->pb_surf, t->width, t->height, t->has_lock, t->init_pid);

    /* Open the frame-signal FIFO (write end).  UI created it O_RDONLY already.
     * O_NONBLOCK: don't block if UI hasn't opened yet (graceful degradation). */
    if (t->frame_fifo_fd < 0) {
        t->frame_fifo_fd = open(PRISM_FB_PATH "-frame.fifo",
                                O_WRONLY | O_NONBLOCK);
        if (t->frame_fifo_fd < 0)
            fprintf(stderr,
                    "[wpe-backend-sdl] frame-signal FIFO open failed: %s\n",
                    strerror(errno));
        else
            fprintf(stderr,
                    "[wpe-backend-sdl] frame-signal FIFO opened (write) fd=%d\n",
                    t->frame_fifo_fd);
    }
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

    /* Create PBuffer + QCOM shared image on first frame when EGL context is current */
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
    /* Guard: eglMakeCurrent with already-current ctx+surf hangs on this driver */
    if (eglGetCurrentSurface(EGL_DRAW) != t->pb_surf) {
        EGLBoolean ok = eglMakeCurrent(t->pb_dpy, t->pb_surf, t->pb_surf, t->pb_ctx);
        fprintf(stderr,
                "[wpe-backend-sdl] switched to PBuffer draw surface: %s (egl=0x%x)\n",
                ok ? "OK" : "FAIL", ok ? 0 : (unsigned)eglGetError());
    }
}

/* ── frame_rendered ─────────────────────────────────────────────────────── */

/*
 * frame_rendered: two paths.
 *
 * Zero-copy path (EGL_QUALCOMM_shared_image, preferred):
 *   WebKit has rendered into the PBuffer (FBO 0).  We GPU-blit those pixels
 *   into the QCOM shared image via glCopyTexSubImage2D:
 *     1. GPU fence sync — wait for WebKit's rendering to finish.
 *     2. Bind the QCOM-backed texture.
 *     3. glCopyTexSubImage2D — reads from current read framebuffer (PBuffer)
 *        and writes directly into the QCOM shared image texture.  Pure GPU
 *        operation: no CPU access, no system-memory bus crossing.
 *     4. glFinish() — ensures all GPU writes are committed before signalling.
 *   CPU is completely out of the pixel path.
 *
 * Fallback path (EGL lock surface → mmap, or glReadPixels → mmap):
 *   Used when QCOM shared image is unavailable (qcom_share_id == 0).
 *   Involves CPU memcpy through system memory — kept as a safety net.
 */
static void
renderer_target_frame_rendered(void *data)
{
    struct sdl_target *t = data;

    /* ── Check for UI's QCOM import ACK ───────────────────────────────────
     * UI sends 4 bytes (0x00000001 = QCOM OK, 0x00000000 = fallback) once,
     * after it attempts eglCreateImageKHR import in on_sdl_poll.
     * Until ACK arrives, qcom_ready stays 0 → we use prism-fb, guaranteeing
     * the page renders.  Once ACK=1, we switch to GPU-only QCOM path.
     */
    if (t->qcom_ack_pending) {
        EGLint ack = 0;
        ssize_t n = recv(t->fd, &ack, sizeof(ack), MSG_DONTWAIT);
        if (n == (ssize_t)sizeof(ack)) {
            t->qcom_ack_pending = 0;
            t->qcom_ready = (ack == 1) ? 1 : 0;
            fprintf(stderr,
                    "[wpe-backend-sdl] QCOM ACK from UI: %s\n",
                    t->qcom_ready ? "QCOM OK — zero-copy active"
                                  : "fallback — staying on prism-fb");
        }
        /* No ACK yet: qcom_ready stays 0, frame uses prism-fb */
    }

    if (t->pb_surf == EGL_NO_SURFACE)
        goto signal;

    /* Ensure PBuffer is the current draw/read surface */
    if (eglGetCurrentSurface(EGL_DRAW) != t->pb_surf)
        eglMakeCurrent(t->pb_dpy, t->pb_surf, t->pb_surf, t->pb_ctx);

    /* ── Step 1: GPU fence — wait for WebKit's rendering to finish ── */
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

    if (t->qcom_ready) {
        /* ── Zero-copy path: GPU blit PBuffer → QCOM shared image ── */
        glBindTexture(GL_TEXTURE_2D, t->qcom_tex);

        /*
         * glCopyTexSubImage2D reads from the current read framebuffer
         * (the PBuffer, FBO 0) and writes into the bound texture's image.
         * Since qcom_tex is backed by the EGL_QUALCOMM_shared_image, this
         * writes directly into the GPU buffer shared with the UI process.
         * No CPU involvement — GPU-only transfer via internal memory.
         *
         * Per GL_OES_EGL_image spec: glCopyTexSubImage2D on an EGLImage-
         * backed texture modifies the shared image's pixel data.
         */
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0,
                            0, 0,               /* xoffset, yoffset in dest */
                            0, 0,               /* x, y in source FB       */
                            (GLsizei)t->width,
                            (GLsizei)t->height);

        GLenum gl_err = glGetError();
        static int qcom_logged = 0;
        if (!qcom_logged) {
            qcom_logged = 1;
            fprintf(stderr,
                    "[wpe-backend-sdl] QCOM GPU blit OK (err=0x%x):"
                    " %ux%u → shared image\n",
                    (unsigned)gl_err, t->width, t->height);
            if (gl_err != GL_NO_ERROR) {
                fprintf(stderr,
                        "[wpe-backend-sdl] glCopyTexSubImage2D failed:"
                        " falling back to prism-fb\n");
                t->qcom_ready = 0;
                goto fallback_readback;
            }
        } else if (gl_err != GL_NO_ERROR) {
            t->qcom_ready = 0;
            goto fallback_readback;
        }

        /*
         * glFinish ensures all GPU writes are committed to the shared image
         * before we signal the UI process.  Since both processes share the
         * same GPU command queue, the UI process's subsequent GL commands
         * that read from the shared image will see the updated data.
         */
        glFinish();

    } else {
fallback_readback:
        if (!t->fb_pixels)
            goto restore;

        if (t->has_lock && t->pfn_LockSurface && t->pfn_UnlockSurface) {
            /* ── Fast fallback: EGL lock surface ── */
            static const EGLint lock_attrs[] = {
                EGL_LOCK_USAGE_HINT_KHR, EGL_READ_SURFACE_BIT_KHR,
                EGL_NONE
            };
            if (t->pfn_LockSurface(t->pb_dpy, t->pb_surf, lock_attrs)) {
                EGLint bitmap_ptr_raw = 0, bitmap_pitch = 0;
                EGLint origin = EGL_UPPER_LEFT_KHR;
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
                        const uint8_t *s = src + (size_t)(t->height-1)*bitmap_pitch;
                        for (uint32_t r = 0; r < t->height;
                             r++, s -= bitmap_pitch, dst += row_bytes)
                            memcpy(dst, s, row_bytes);
                    } else {
                        if ((size_t)bitmap_pitch == row_bytes) {
                            memcpy(dst, src, t->fb_size);
                        } else {
                            const uint8_t *s = src;
                            for (uint32_t r = 0; r < t->height;
                                 r++, s += bitmap_pitch, dst += row_bytes)
                                memcpy(dst, s, row_bytes);
                        }
                    }
                    static int lock_logged = 0;
                    if (!lock_logged) {
                        lock_logged = 1;
                        fprintf(stderr,
                                "[wpe-backend-sdl] lock readback OK:"
                                " origin=%s pitch=%d\n",
                                origin == EGL_LOWER_LEFT_KHR
                                    ? "lower-left" : "upper-left",
                                bitmap_pitch);
                    }
                }
                t->pfn_UnlockSurface(t->pb_dpy, t->pb_surf);
            } else {
                fprintf(stderr,
                        "[wpe-backend-sdl] eglLockSurface failed (0x%x),"
                        " falling back to glReadPixels\n",
                        (unsigned)eglGetError());
                t->has_lock = 0;
                goto readpixels;
            }
        } else {
readpixels:
            glReadPixels(0, 0, (GLsizei)t->width, (GLsizei)t->height,
                         GL_RGBA, GL_UNSIGNED_BYTE, t->fb_pixels);
            static int rp_logged = 0;
            if (!rp_logged) {
                rp_logged = 1;
                fprintf(stderr, "[wpe-backend-sdl] glReadPixels fallback\n");
            }
        }
    }

restore:
    /*
     * Do NOT restore win_surf here.  WPE calls eglSwapBuffers after
     * frame_rendered — if win_surf is current, that swap presents a blank
     * PDK fullscreen surface and SysMgr hands display ownership to
     * WPEWebProcess, overwriting SDL's composited output.  For static pages
     * (info.cern.ch) SDL recovers quickly; for live pages (google.com) WP
     * keeps swapping and the display never shows SDL's frame.
     *
     * Keeping pb_surf current means WPE's eglSwapBuffers hits the PBuffer —
     * a no-op for display output.  frame_will_render already guards against
     * redundant eglMakeCurrent (checks pb_surf == current before calling).
     */

signal:
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->wpe_target);

    /* Signal UI via FIFO — works across all WebProcesses regardless of IPC fd.
     * t->fd is EBADF for second+ WebProcesses so we don't use it for signals. */
    if (t->frame_fifo_fd >= 0) {
        const char sig = 'F';
        write(t->frame_fifo_fd, &sig, 1);
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
