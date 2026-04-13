#pragma once

#include <wpe/wpe.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stddef.h>

#define WEBOS_SCREEN_W 1024
#define WEBOS_SCREEN_H 768
#define CHROME_H       64
#define WEBKIT_H       (WEBOS_SCREEN_H - CHROME_H)   /* 704 */
#define PRISM_FB_PATH  "/tmp/prism-fb"

/* ── Chrome toolbar layout ───────────────────────────────────────────────── */
#define CHROME_BTN_Y       10
#define CHROME_BTN_H       44
#define CHROME_BACK_X       8
#define CHROME_FWD_X       62
#define CHROME_RELOAD_X   116
#define CHROME_BTN_W       44
#define CHROME_URL_X      172
#define CHROME_URL_W      852
#define CHROME_URL_H       40
#define CHROME_URL_Y       12

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

/* ── View backend (UI process side) ─────────────────────────────────────── */
struct sdl_view_backend {
    struct wpe_view_backend *wpe_backend;

    int host_fd;
    int renderer_fd;

    int     touch_active;
    int32_t touch_x;
    int32_t touch_y;

    /* Shared-memory framebuffer written by WPEWebProcess */
    int    fb_fd;
    void  *fb_pixels;
    size_t fb_size;

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

    /* Chrome toolbar */
    unsigned char chrome_pixels[CHROME_H * WEBOS_SCREEN_W * 4];
    int  chrome_dirty;
    int  url_editing;
    char url_current[2048];
    char url_edit[2048];
    int  can_go_back;
    int  can_go_forward;
    int  loading;

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

    /* Navigation callbacks wired from main.c via dlsym API */
    void (*navigate_cb)(const char *url, void *user);
    void (*go_back_cb)(void *user);
    void (*go_fwd_cb)(void *user);
    void (*reload_cb)(void *user);
    void (*stop_cb)(void *user);
    void  *nav_user;
};

extern const struct wpe_view_backend_interface sdl_view_backend_interface;
struct sdl_view_backend *wpe_sdl_vb_get(void);
