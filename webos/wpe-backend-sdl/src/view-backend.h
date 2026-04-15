#pragma once

#include <wpe/wpe.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>   /* EGLImageKHR, PFNEGLDESTROYIMAGEKHRPROC, EGL_NO_IMAGE_KHR */
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stddef.h>

/* GL_OES_EGL_image types — PDK glextplatform.h disables this extension unless
 * PRE or PIXI is defined.  Forward-declare the types we need ourselves so
 * cross-compilation works without setting platform-specific build flags.
 * These are compatible with what glext.h would provide when enabled. */
#ifndef GLeglImageOES
typedef void *GLeglImageOES;
#define GLeglImageOES GLeglImageOES
#endif
#ifndef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);
#endif

#define CHROME_H       64
#define PRISM_FB_PATH  "/tmp/prism-fb"

/* ── Chrome toolbar layout (fixed pixel values, independent of screen size) ─ */
#define CHROME_BTN_Y       10
#define CHROME_BTN_H       44
#define CHROME_BACK_X       8
#define CHROME_FWD_X       62
#define CHROME_RELOAD_X   116
#define CHROME_HOME_X     164   /* home button — after reload (116+44+4=164)   */
#define CHROME_BTN_W       44
#define CHROME_URL_X      212   /* URL bar left edge — right edge is screen_w-4 */
#define CHROME_URL_H       40
#define CHROME_URL_Y       12
/* CHROME_URL_W is runtime: vb->screen_w - CHROME_URL_X - 4 */

/* ── On-screen keyboard ──────────────────────────────────────────────────── */
#define KB_H          270          /* keyboard height in pixels              */
#define KB_MAX_KEYS    80          /* max keys across all rows               */

/* Special key codes */
#define KB_NONE        0
#define KB_BACKSPACE   1
#define KB_ENTER       2           /* navigate / dismiss                     */
#define KB_SHIFT       3
#define KB_SPACE       4
#define KB_DOTCOM      5           /* inserts ".com"                         */
#define KB_DISMISS     6           /* hide keyboard without navigating       */

/* ── History dropdown ────────────────────────────────────────────────────── */
#define HIST_MAX         50   /* entries kept in file + memory              */
#define HIST_SHOW         5   /* max rows shown in dropdown                 */
#define HIST_ROW_H       44   /* height of each dropdown row (px)           */

/* ── Bookmark star button ────────────────────────────────────────────────── */
#define CHROME_STAR_W       44   /* star button (rightmost)                 */
#define CHROME_BMARK_NAV_W  36   /* bookmark-list nav button                */
#define CHROME_ZOOM_W       36   /* zoom-in / zoom-out buttons              */
#define BMARK_MAX           50   /* max bookmarks                           */

/* One key in the keyboard layout spec */
typedef struct {
    const char *label;   /* displayed text                                   */
    const char *insert;  /* text inserted into url_edit; NULL if special     */
    int         special; /* KB_* code, 0 for regular insert                  */
    float       w_mul;   /* width multiplier relative to base key width      */
} KbSpec;

/* A rendered key cell (pixel rect + spec pointer) */
typedef struct {
    int          x, y, w, h;  /* position inside keyboard pixel buffer      */
    const KbSpec *spec;
} KbCell;

/* ── Download panel ──────────────────────────────────────────────────────── */
#define DL_MAX_ENTRIES    8    /* max tracked download slots                  */
#define DL_PANEL_W      360    /* panel pixel width                           */
#define DL_PANEL_HDR_H   34    /* header row height                           */
#define DL_ROW_H         54    /* height per download row                     */

#define DL_STATUS_FREE   (-1)  /* slot not in use                             */
#define DL_STATUS_ACTIVE   0
#define DL_STATUS_DONE     1
#define DL_STATUS_FAILED   2

typedef struct {
    char  filename[256];
    float progress;            /* 0.0–1.0                                     */
    int   status;              /* DL_STATUS_*                                 */
} DlEntry;

/* ── EGL_QUALCOMM_shared_image tokens (not in PDK headers) ──────────────── */
#define EGL_SHARED_IMAGE_QCOM      0x3120  /* eglCreateImageKHR target: import */
#define EGL_SHARED_IMAGE_ID_QCOM   0x3121  /* attrib key: integer share handle */

/* EGL_QUALCOMM_shared_image function pointer type (obtained via eglGetProcAddress).
 *
 * Reverse-engineered from libEGL.so disassembly: the function has FOUR arguments.
 *   arg2 (fmt_attrs):  small-integer pixel-format attribute list (NULL = default).
 *   arg3 (sz_attrs):   standard EGL_HEIGHT/EGL_WIDTH attribute list.
 * The three-argument prototype in public documentation is wrong.
 */
typedef EGLImageKHR (*PFNEGLCREATESHAREDIMAGEQUALCOMMPROC)(
    EGLDisplay dpy, EGLContext ctx,
    const EGLint *fmt_attrs,   /* format attrs — small-int keys, NULL for default */
    const EGLint *sz_attrs);   /* size attrs   — EGL_HEIGHT, EGL_WIDTH, EGL_NONE */

/* ── View backend (UI process side) ─────────────────────────────────────── */
struct sdl_view_backend {
    struct wpe_view_backend *wpe_backend;

    int host_fd;
    int renderer_fd;

    int     touch_active;
    int32_t touch_x;
    int32_t touch_y;

    /* ── Zero-copy path: EGL_QUALCOMM_shared_image ───────────────────────
     * WPEWebProcess creates the shared GPU image on its own EGL display
     * and sends {share_id, dpy} to UI over the socket.  UI imports it
     * via eglCreateImageKHR(0x3120) and binds it as blit_tex.  WPEWebProcess
     * GPU-blits each rendered frame into it; UI reads via glEGLImageTargetTex
     * each repaint.  No CPU copy, no mmap, no glTexImage2D needed.
     */
    EGLImageKHR qcom_shared_image; /* imported from WPEWebProcess            */
    int         qcom_ready;        /* 1 when zero-copy path is active         */
    int         qcom_received;     /* 1 once 8-byte QCOM init packet read     */

    /* EGL/GL extension functions (loaded once after SDL EGL init) */
    PFNEGLCREATEIMAGEKHRPROC              pfn_CreateImage;   /* for import  */
    PFNEGLDESTROYIMAGEKHRPROC            pfn_DestroyImage;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC   pfn_ImageTargetTex;

    /* ── Fallback: CPU mmap path (prism-fb) ──────────────────────────────
     * Used when QCOM shared image creation fails.
     */
    int    fb_fd;
    void  *fb_pixels;
    size_t fb_size;
    int    frame_fifo_fd;  /* read end of /tmp/prism-frame.fifo — all WPs write 'F' here */

    /* GLES2 programs + textures */
    GLuint blit_prog;    /* RGBA — for WebKit pixels       */
    GLuint cairo_prog;   /* BGRA→RGBA — for cairo surfaces */
    GLuint blit_tex;
    GLuint chrome_tex;
    GLuint kb_tex;
    int    blit_ready;
    int    webkit_dirty;  /* new frame arrived from WPEWebProcess */

    /* EGL context saved after SDL_SetVideoMode — re-asserted in repaint() */
    EGLDisplay sdl_egl_dpy;
    EGLSurface sdl_egl_surf;
    EGLContext sdl_egl_ctx;

    /* Runtime screen dimensions (updated on init and on SDL_VIDEORESIZE) */
    int screen_w;   /* current SDL window width  */
    int screen_h;   /* current SDL window height */
    int webkit_h;   /* screen_h - CHROME_H       */

    /* Chrome toolbar */
    unsigned char *chrome_pixels;  /* malloc'd: CHROME_H * screen_w * 4 */
    int  chrome_dirty;
    int  url_editing;
    char url_current[2048];
    char url_edit[2048];
    int  can_go_back;
    int  can_go_forward;
    int  loading;
    float load_progress;  /* 0.0–1.0; drawn as fill behind URL text */

    /* Favicon (20×20 ARGB32 cairo surface owned by backend; NULL = none) */
    void *favicon_surf;

    /* URL History */
    char            history[HIST_MAX][2048];
    int             history_count;
    /* History dropdown */
    char            hist_rows[HIST_SHOW][2048];
    int             hist_nrows;
    int             hist_visible;
    unsigned char  *hist_pixels;   /* hist_nrows*HIST_ROW_H × drop_w × 4 */
    GLuint          hist_tex;
    int             hist_dirty;

    /* Bookmarks */
    char  bookmarks[BMARK_MAX][2048];
    int   bookmark_count;
    int   is_bookmarked;   /* 1 = current URL is bookmarked */

    /* On-screen keyboard (cairo URL-bar keyboard only) */
    unsigned char *kb_pixels;   /* malloc'd: KB_H * WEBOS_SCREEN_W * 4 */
    KbCell         kb_cells[KB_MAX_KEYS];
    int            kb_ncells;
    int            kb_visible;  /* URL-bar cairo keyboard               */
    int            kb_shift;    /* 0=lower, 1=upper (one-shot)          */
    int            kb_dirty;

    /* Native webOS PDL keyboard (for web content input fields) */
    int            pdl_kb_visible;
    int            touch_was_scroll;  /* did last touch sequence move? */

    /* Download panel */
    DlEntry        dl_entries[DL_MAX_ENTRIES];
    int            dl_visible;        /* panel shown                          */
    unsigned char *dl_pixels;         /* DL_PANEL_W × dl_panel_h × 4         */
    int            dl_panel_h;        /* current rendered panel height        */
    GLuint         dl_tex;
    int            dl_dirty;
    uint32_t       dl_done_at_ms;     /* when all finished — for auto-hide    */

    /* Navigation callbacks wired from main.c via dlsym API */
    void (*navigate_cb)(const char *url, void *user);
    void (*go_back_cb)(void *user);
    void (*go_fwd_cb)(void *user);
    void (*reload_cb)(void *user);
    void (*stop_cb)(void *user);
    void (*zoom_in_cb)(void *user);
    void (*zoom_out_cb)(void *user);
    void (*home_cb)(void *user);
    void (*blur_input_cb)(void *user);  /* blur focused web element + hide PDL kb */
    void  *nav_user;
};

extern const struct wpe_view_backend_interface sdl_view_backend_interface;
struct sdl_view_backend *wpe_sdl_vb_get(void);
