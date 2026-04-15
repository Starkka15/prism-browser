#pragma once

#include <wpe/wpe-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES/glext.h>
#include <stdint.h>
#include <stddef.h>

/* GL_OES_EGL_image types — PDK glextplatform.h disables this extension guard
 * unless PRE or PIXI is defined at build time.  Forward-declare what we need. */
#ifndef GLeglImageOES
typedef void *GLeglImageOES;
#define GLeglImageOES GLeglImageOES
#endif
#ifndef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
#endif

/* EGL_QUALCOMM_shared_image creator (reverse-engineered 4-arg signature).
 * Duplicated here so backend-egl.h is self-contained (view-backend.h has
 * the same typedef guarded identically). */
#ifndef PFNEGLCREATESHAREDIMAGEQUALCOMMPROC
typedef EGLImageKHR (*PFNEGLCREATESHAREDIMAGEQUALCOMMPROC)(
    EGLDisplay dpy, EGLContext ctx,
    const EGLint *fmt_attrs,
    const EGLint *sz_attrs);
#define PFNEGLCREATESHAREDIMAGEQUALCOMMPROC PFNEGLCREATESHAREDIMAGEQUALCOMMPROC
#endif

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

    /* ── Zero-copy path: EGL_QUALCOMM_shared_image ───────────────────────
     * WP (WebContent process) creates the shared image on its own EGL
     * display+context and sends {share_id, dpy} to the UI process.
     * UI imports it via eglCreateImageKHR(0x3120) and reads from it.
     * WP GPU-blits each rendered PBuffer frame into qcom_tex via
     * glCopyTexSubImage2D — pure GPU, no CPU copy involved.
     */
    EGLint      qcom_share_id;       /* (EGLint)qcom_created_image handle     */
    EGLImageKHR qcom_created_image;  /* image created by WP on pb_dpy         */
    GLuint      qcom_tex;            /* GL texture backed by the shared image  */
    int         qcom_ready;          /* 1 when zero-copy path is active        */
    int         qcom_ack_pending;    /* 1 while awaiting UI import ACK        */

    /* EGL/GL extension function pointers for QCOM zero-copy path */
    PFNEGLCREATESHAREDIMAGEQUALCOMMPROC         pfn_CreateSharedImage_wp;
    PFNEGLCREATEIMAGEKHRPROC                    pfn_CreateImage;
    PFNEGLDESTROYIMAGEKHRPROC                   pfn_DestroyImage;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC         pfn_ImageTargetTex;

    /* ── Fallback: CPU mmap path (prism-fb) ──────────────────────────────
     * Used when qcom_share_id == 0 (UI process fell back to prism-fb).
     * Shared-memory framebuffer written via lock-surface or glReadPixels.
     */
    int    fb_fd;
    void  *fb_pixels;
    size_t fb_size;

    /*
     * EGL PBuffer surface used as the WebKit draw target.
     *
     * The PDK EGL window surface (NativeWindowType=0) is write-only — glReadPixels
     * on it returns GL_INVALID_VALUE.  In frame_will_render we redirect the current
     * EGL draw surface to pb_surf (a PBuffer).  WebKit renders into it via FBO 0.
     * In frame_rendered we either GPU-blit into the QCOM shared image (zero-copy)
     * or fall back to lock-surface / glReadPixels into the mmap file.
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

    /* PID that created pb_surf — used to detect stale state when WPE reuses
     * this target struct across WebProcess respawns (no destroy/create cycle). */
    int init_pid;

    /* Write end of /tmp/prism-frame.fifo — 'F' goes here instead of t->fd
     * so all WebProcesses signal UI on the same channel regardless of IPC fd. */
    int frame_fifo_fd;
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
