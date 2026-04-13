/*
 * wpe-backend-sdl: View backend + Chrome toolbar + On-screen keyboard
 * HP TouchPad (webOS 3.0.5, SDL 1.2, PowerVR SGX540)
 *
 * Screen layout:
 *   y=0      ┌────────────────────┐
 *             │  Chrome bar (64px) │  Cairo ARGB32 → cairo_prog (BGRA swap)
 *   y=64     ├────────────────────┤
 *             │                    │
 *             │  WebKit (704px)    │  RGBA from PBuffer → blit_prog
 *             │                    │
 *   y=768    └────────────────────┘
 *
 * When keyboard visible, it overlays the bottom 270px of the WebKit area:
 *   y=498    ┌────────────────────┐
 *             │  Keyboard (270px)  │  Cairo ARGB32 → cairo_prog
 *   y=768    └────────────────────┘
 */

#include "view-backend.h"
#include "keysym-map.h"

#include <wpe/wpe.h>
#include <SDL/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cairo.h>
#include <glib.h>
#include <PDL.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <math.h>

static struct sdl_view_backend *g_vb = NULL;
struct sdl_view_backend *wpe_sdl_vb_get(void) { return g_vb; }

/* ── Helpers ────────────────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* ── WPE input dispatch ─────────────────────────────────────────────────── */

static void dispatch_key(struct sdl_view_backend *vb, SDL_KeyboardEvent *ke)
{
    struct wpe_input_keyboard_event ev = {
        .time              = now_ms(),
        .key_code          = sdlkey_to_wpe(ke->keysym.sym),
        .hardware_key_code = (uint32_t)ke->keysym.scancode,
        .pressed           = (ke->type == SDL_KEYDOWN),
        .modifiers         = sdl_mod_to_wpe(ke->keysym.mod),
    };
    wpe_view_backend_dispatch_keyboard_event(vb->wpe_backend, &ev);
}

static void dispatch_touch(struct sdl_view_backend *vb, int x, int y,
                            enum wpe_input_touch_event_type type)
{
    struct wpe_input_touch_event_raw raw = {
        .type = type, .time = now_ms(), .id = 0, .x = x, .y = y,
    };
    struct wpe_input_touch_event ev = {
        .touchpoints = &raw, .touchpoints_length = 1,
        .type = type, .id = 0, .time = raw.time, .modifiers = 0,
    };
    wpe_view_backend_dispatch_touch_event(vb->wpe_backend, &ev);
}

static void dispatch_scroll(struct sdl_view_backend *vb, SDL_MouseButtonEvent *be)
{
    int delta = (be->button == SDL_BUTTON_WHEELUP) ? -120 : 120;
    struct wpe_input_axis_event ev = {
        .type = wpe_input_axis_event_type_motion, .time = now_ms(),
        .x = be->x, .y = be->y - CHROME_H,
        .axis = 1, .value = delta, .modifiers = 0,
    };
    wpe_view_backend_dispatch_axis_event(vb->wpe_backend, &ev);
}

/* ════════════════════════════════════════════════════════════════════════════
 * CHROME TOOLBAR
 * ════════════════════════════════════════════════════════════════════════════ */

static void rounded_rect(cairo_t *cr, double x, double y,
                          double w, double h, double r)
{
    cairo_new_path(cr);
    cairo_arc(cr, x+r,     y+r,     r, M_PI,      3*M_PI/2);
    cairo_arc(cr, x+w-r,   y+r,     r, 3*M_PI/2,  2*M_PI);
    cairo_arc(cr, x+w-r,   y+h-r,   r, 0,          M_PI/2);
    cairo_arc(cr, x+r,     y+h-r,   r, M_PI/2,     M_PI);
    cairo_close_path(cr);
}

static void draw_back_icon(cairo_t *cr, double cx, double cy,
                            double sz, int enabled)
{
    cairo_set_source_rgb(cr, enabled ? 0.85 : 0.30, enabled ? 0.85 : 0.30,
                             enabled ? 0.85 : 0.30);
    cairo_new_path(cr);
    cairo_move_to(cr, cx - sz*0.45, cy);
    cairo_line_to(cr, cx + sz*0.35, cy - sz*0.40);
    cairo_line_to(cr, cx + sz*0.35, cy + sz*0.40);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_fwd_icon(cairo_t *cr, double cx, double cy,
                           double sz, int enabled)
{
    cairo_set_source_rgb(cr, enabled ? 0.85 : 0.30, enabled ? 0.85 : 0.30,
                             enabled ? 0.85 : 0.30);
    cairo_new_path(cr);
    cairo_move_to(cr, cx + sz*0.45, cy);
    cairo_line_to(cr, cx - sz*0.35, cy - sz*0.40);
    cairo_line_to(cr, cx - sz*0.35, cy + sz*0.40);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_reload_icon(cairo_t *cr, double cx, double cy,
                              double sz, int is_loading)
{
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_set_line_width(cr, sz * 0.12);
    if (is_loading) {
        double d = sz * 0.30;
        cairo_new_path(cr);
        cairo_move_to(cr, cx-d, cy-d); cairo_line_to(cr, cx+d, cy+d);
        cairo_move_to(cr, cx+d, cy-d); cairo_line_to(cr, cx-d, cy+d);
        cairo_stroke(cr);
    } else {
        double r = sz * 0.34;
        double sa = 120.0*M_PI/180.0, ea = 390.0*M_PI/180.0;
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, r, sa, ea);
        cairo_stroke(cr);
        double ax = cx + r*cos(ea), ay = cy + r*sin(ea);
        double tg = ea + M_PI/2.0;
        double head = sz * 0.18;
        cairo_new_path(cr);
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, ax + head*cos(tg-0.5), ay + head*sin(tg-0.5));
        cairo_line_to(cr, ax + head*cos(tg+0.5), ay + head*sin(tg+0.5));
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

static void chrome_render(struct sdl_view_backend *vb)
{
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        vb->chrome_pixels, CAIRO_FORMAT_ARGB32,
        WEBOS_SCREEN_W, CHROME_H, WEBOS_SCREEN_W * 4);
    cairo_t *cr = cairo_create(surf);

    cairo_set_source_rgb(cr, 0.114, 0.114, 0.114);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, CHROME_H - 0.5);
    cairo_line_to(cr, WEBOS_SCREEN_W, CHROME_H - 0.5);
    cairo_stroke(cr);

    double btn_cx[3] = {
        CHROME_BACK_X   + CHROME_BTN_W/2.0,
        CHROME_FWD_X    + CHROME_BTN_W/2.0,
        CHROME_RELOAD_X + CHROME_BTN_W/2.0,
    };
    double btn_cy = CHROME_BTN_Y + CHROME_BTN_H/2.0;

    for (int i = 0; i < 3; i++) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.04);
        rounded_rect(cr, btn_cx[i]-CHROME_BTN_W/2.0, CHROME_BTN_Y,
                     CHROME_BTN_W, CHROME_BTN_H, 6.0);
        cairo_fill(cr);
    }

    draw_back_icon  (cr, btn_cx[0], btn_cy, 20.0, vb->can_go_back);
    draw_fwd_icon   (cr, btn_cx[1], btn_cy, 20.0, vb->can_go_forward);
    draw_reload_icon(cr, btn_cx[2], btn_cy, 20.0, vb->loading);

    rounded_rect(cr, CHROME_URL_X, CHROME_URL_Y,
                 CHROME_URL_W, CHROME_URL_H, 6.0);
    cairo_set_source_rgb(cr, 0.196, 0.196, 0.196);
    cairo_fill_preserve(cr);
    if (vb->url_editing)
        cairo_set_source_rgb(cr, 0.290, 0.565, 0.847);
    else
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 18.0);

    const char *display = vb->url_editing ? vb->url_edit : vb->url_current;

    cairo_save(cr);
    rounded_rect(cr, CHROME_URL_X+2, CHROME_URL_Y+2,
                 CHROME_URL_W-4, CHROME_URL_H-4, 5.0);
    cairo_clip(cr);

    cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
    cairo_move_to(cr, CHROME_URL_X + 12.0, CHROME_URL_Y + CHROME_URL_H/2.0 + 7.0);
    cairo_show_text(cr, display[0] ? display : "about:blank");

    if (vb->url_editing) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, display, &ext);
        double cx2 = CHROME_URL_X + 12.0 + ext.x_advance + 2.0;
        cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, cx2, CHROME_URL_Y + 8.0);
        cairo_line_to(cr, cx2, CHROME_URL_Y + CHROME_URL_H - 8.0);
        cairo_stroke(cr);
    }
    cairo_restore(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    vb->chrome_dirty = 0;
}

static void chrome_handle_touch(struct sdl_view_backend *vb, int x, int y)
{
    (void)y;
    if (x >= CHROME_BACK_X && x < CHROME_BACK_X + CHROME_BTN_W) {
        if (vb->can_go_back && vb->go_back_cb)
            vb->go_back_cb(vb->nav_user);
    } else if (x >= CHROME_FWD_X && x < CHROME_FWD_X + CHROME_BTN_W) {
        if (vb->can_go_forward && vb->go_fwd_cb)
            vb->go_fwd_cb(vb->nav_user);
    } else if (x >= CHROME_RELOAD_X && x < CHROME_RELOAD_X + CHROME_BTN_W) {
        if (vb->loading && vb->stop_cb)        vb->stop_cb(vb->nav_user);
        else if (!vb->loading && vb->reload_cb) vb->reload_cb(vb->nav_user);
    } else if (x >= CHROME_URL_X && x < CHROME_URL_X + CHROME_URL_W) {
        if (!vb->url_editing) {
            vb->url_editing = 1;
            strncpy(vb->url_edit, vb->url_current, sizeof(vb->url_edit)-1);
            vb->url_edit[sizeof(vb->url_edit)-1] = '\0';
            PDL_SetKeyboardState(PDL_TRUE);
        }
        vb->chrome_dirty = 1;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * ON-SCREEN KEYBOARD
 * ════════════════════════════════════════════════════════════════════════════ */

/* Layout spec: 5 rows of keys.  NULL label = end of row sentinel. */
static const KbSpec kb_row0[] = {
    {"q","q",0,1},{"w","w",0,1},{"e","e",0,1},{"r","r",0,1},{"t","t",0,1},
    {"y","y",0,1},{"u","u",0,1},{"i","i",0,1},{"o","o",0,1},{"p","p",0,1},
    {"⌫",NULL,KB_BACKSPACE,1.5},
    {NULL,NULL,0,0}
};
static const KbSpec kb_row1[] = {
    {"a","a",0,1},{"s","s",0,1},{"d","d",0,1},{"f","f",0,1},{"g","g",0,1},
    {"h","h",0,1},{"j","j",0,1},{"k","k",0,1},{"l","l",0,1},
    {"Go",NULL,KB_ENTER,1.5},
    {NULL,NULL,0,0}
};
static const KbSpec kb_row2[] = {
    {"z","z",0,1},{"x","x",0,1},{"c","c",0,1},{"v","v",0,1},{"b","b",0,1},
    {"n","n",0,1},{"m","m",0,1},{"@","@",0,1},{".com",NULL,KB_DOTCOM,1.8},
    {NULL,NULL,0,0}
};
static const KbSpec kb_row3[] = {
    {"1","1",0,1},{"2","2",0,1},{"3","3",0,1},{"4","4",0,1},{"5","5",0,1},
    {"6","6",0,1},{"7","7",0,1},{"8","8",0,1},{"9","9",0,1},{"0","0",0,1},
    {NULL,NULL,0,0}
};
static const KbSpec kb_row4[] = {
    {"⇧",NULL,KB_SHIFT,1.2},
    {"-","-",0,1},{".",".",0,1},{"/","/",0,1},{":",":",0,1},{"?","?",0,1},
    {"=","=",0,1},
    {"         ",NULL,KB_SPACE,3.5},
    {"#","#",0,1},{"_","_",0,1},
    {"×",NULL,KB_DISMISS,1.2},
    {NULL,NULL,0,0}
};

static const KbSpec *kb_rows[5] = {kb_row0, kb_row1, kb_row2, kb_row3, kb_row4};

/* Build KbCell array with computed pixel rects.
 * Called once in view_backend_initialize. */
static void kb_init(struct sdl_view_backend *vb)
{
    const int pad_x  = 5;
    const int pad_y  = 4;
    const int row_h  = 50;
    const int row_gap = 4;
    const int key_gap = 3;
    const int avail_w = WEBOS_SCREEN_W - 2*pad_x;

    vb->kb_ncells = 0;

    for (int row = 0; row < 5; row++) {
        const KbSpec *specs = kb_rows[row];

        /* Count total width units for this row */
        float total_units = 0.0f;
        int nkeys = 0;
        for (int k = 0; specs[k].label != NULL; k++) {
            total_units += specs[k].w_mul;
            nkeys++;
        }
        if (nkeys == 0) continue;

        /* Base key width: fill avail_w accounting for gaps */
        float base_w = (avail_w - (nkeys-1)*key_gap) / total_units;
        int row_y = pad_y + row * (row_h + row_gap);

        float x = pad_x;
        for (int k = 0; specs[k].label != NULL; k++) {
            if (vb->kb_ncells >= KB_MAX_KEYS) break;
            float w = base_w * specs[k].w_mul;
            KbCell *cell = &vb->kb_cells[vb->kb_ncells++];
            cell->x    = (int)(x + 0.5f);
            cell->y    = row_y;
            cell->w    = (int)(w + 0.5f);
            cell->h    = row_h;
            cell->spec = &specs[k];
            x += w + key_gap;
        }
    }
}

/* Render keyboard into kb_pixels using cairo */
static void kb_render(struct sdl_view_backend *vb)
{
    if (!vb->kb_pixels) return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        vb->kb_pixels, CAIRO_FORMAT_ARGB32,
        WEBOS_SCREEN_W, KB_H, WEBOS_SCREEN_W * 4);
    cairo_t *cr = cairo_create(surf);

    /* Background */
    cairo_set_source_rgb(cr, 0.137, 0.137, 0.137);
    cairo_paint(cr);

    /* Top separator */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, 0.5);
    cairo_line_to(cr, WEBOS_SCREEN_W, 0.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    for (int i = 0; i < vb->kb_ncells; i++) {
        const KbCell *cell = &vb->kb_cells[i];
        const KbSpec *spec = cell->spec;
        double kx = cell->x, ky = cell->y;
        double kw = cell->w, kh = cell->h;

        int is_special = (spec->special != KB_NONE);
        int is_shift   = (spec->special == KB_SHIFT && vb->kb_shift);
        int is_enter   = (spec->special == KB_ENTER || spec->special == KB_DISMISS);

        /* Key background */
        if (is_shift && vb->kb_shift)
            cairo_set_source_rgb(cr, 0.290, 0.565, 0.847);
        else if (is_enter)
            cairo_set_source_rgb(cr, 0.220, 0.450, 0.700);
        else if (is_special)
            cairo_set_source_rgb(cr, 0.200, 0.200, 0.200);
        else
            cairo_set_source_rgb(cr, 0.310, 0.310, 0.310);

        rounded_rect(cr, kx+1, ky+1, kw-2, kh-2, 5.0);
        cairo_fill(cr);

        /* Key label */
        cairo_set_font_size(cr, (kw > 60) ? 16.0 : 18.0);

        /* For letter keys, apply shift */
        char shifted[8] = {0};
        const char *lbl = spec->label;
        if (!is_special && spec->insert && strlen(spec->insert)==1
                && isalpha((unsigned char)spec->insert[0]) && vb->kb_shift) {
            shifted[0] = toupper((unsigned char)spec->insert[0]);
            lbl = shifted;
        }

        cairo_text_extents_t ext;
        cairo_text_extents(cr, lbl, &ext);
        double tx = kx + (kw - ext.x_advance) / 2.0;
        double ty = ky + (kh + ext.height) / 2.0 - ext.y_bearing - ext.height;

        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, lbl);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    vb->kb_dirty = 0;
}

/* Hit-test: find key at keyboard-relative (kx, ky).  Returns NULL if none. */
static const KbCell *kb_hit(struct sdl_view_backend *vb, int kx, int ky)
{
    for (int i = 0; i < vb->kb_ncells; i++) {
        const KbCell *c = &vb->kb_cells[i];
        if (kx >= c->x && kx < c->x+c->w &&
            ky >= c->y && ky < c->y+c->h)
            return c;
    }
    return NULL;
}

static void kb_handle_touch(struct sdl_view_backend *vb, int sx, int sy)
{
    /* sy is screen-absolute; convert to keyboard-relative */
    int kb_y0 = WEBOS_SCREEN_H - KB_H;
    const KbCell *cell = kb_hit(vb, sx, sy - kb_y0);
    if (!cell) return;

    const KbSpec *spec = cell->spec;
    size_t len = strlen(vb->url_edit);

    switch (spec->special) {
    case KB_NONE:
        /* Regular character insert */
        if (spec->insert && len < sizeof(vb->url_edit)-1) {
            if (strlen(spec->insert)==1 && isalpha((unsigned char)spec->insert[0])
                    && vb->kb_shift) {
                vb->url_edit[len] = toupper((unsigned char)spec->insert[0]);
                vb->kb_shift = 0;
            } else {
                strcat(vb->url_edit, spec->insert);
            }
            vb->url_edit[sizeof(vb->url_edit)-1] = '\0';
        }
        break;

    case KB_BACKSPACE:
        if (len > 0) vb->url_edit[len-1] = '\0';
        break;

    case KB_ENTER: {
        /* Navigate and dismiss */
        char buf[2100];
        const char *url = vb->url_edit;
        if (url[0] && !strchr(url, ':')) {
            snprintf(buf, sizeof(buf), "https://%s", url);
            url = buf;
        }
        if (vb->navigate_cb && url[0])
            vb->navigate_cb(url, vb->nav_user);
        vb->url_editing = 0;
        PDL_SetKeyboardState(PDL_FALSE);
        break;
    }

    case KB_SHIFT:
        vb->kb_shift = !vb->kb_shift;
        break;

    case KB_SPACE:
        if (len < sizeof(vb->url_edit)-1) {
            vb->url_edit[len]   = ' ';
            vb->url_edit[len+1] = '\0';
        }
        break;

    case KB_DOTCOM:
        if (len + 4 < sizeof(vb->url_edit)) {
            strcat(vb->url_edit, ".com");
            vb->url_edit[sizeof(vb->url_edit)-1] = '\0';
        }
        break;

    case KB_DISMISS:
        vb->url_editing = 0;
        PDL_SetKeyboardState(PDL_FALSE);
        break;
    }

    vb->chrome_dirty = 1;
    vb->kb_dirty     = 1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * SDL EVENT PUMP
 * ════════════════════════════════════════════════════════════════════════════ */

static void dismiss_keyboard(struct sdl_view_backend *vb)
{
    if (vb->url_editing) {
        vb->url_editing  = 0;
        vb->chrome_dirty = 1;
        PDL_SetKeyboardState(PDL_FALSE);
    }
}

static void repaint(struct sdl_view_backend *vb); /* forward decl */

static void pump_sdl_events(struct sdl_view_backend *vb)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            /* Physical keyboard — feed into URL bar if editing, else WebKit */
            if (vb->url_editing && e.type == SDL_KEYDOWN) {
                SDLKey sym = e.key.keysym.sym;
                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                    char buf[2100];
                    const char *url = vb->url_edit;
                    if (url[0] && !strchr(url, ':')) {
                        snprintf(buf, sizeof(buf), "https://%s", url);
                        url = buf;
                    }
                    if (vb->navigate_cb && url[0])
                        vb->navigate_cb(url, vb->nav_user);
                    dismiss_keyboard(vb);
                } else if (sym == SDLK_ESCAPE) {
                    dismiss_keyboard(vb);
                } else if (sym == SDLK_BACKSPACE) {
                    size_t len = strlen(vb->url_edit);
                    if (len > 0) vb->url_edit[len-1] = '\0';
                    vb->chrome_dirty = 1;
                } else {
                    /* Use SDL unicode if populated; otherwise synthesize from
                     * sym + modifier (PDL keyboard may not set unicode). */
                    Uint16 uc = e.key.keysym.unicode;
                    SDLMod mod = e.key.keysym.mod;
                    int shifted = (mod & (KMOD_SHIFT | KMOD_CAPS)) != 0;
                    if (uc == 0) {
                        if (sym >= SDLK_a && sym <= SDLK_z)
                            uc = shifted ? (sym - 32) : sym;
                        else if (sym >= SDLK_SPACE && sym <= SDLK_DELETE)
                            uc = (Uint16)sym; /* covers digits, punct */
                    } else if (uc >= 'a' && uc <= 'z' && shifted) {
                        uc -= 32; /* unicode lowercase but shift held */
                    }
                    if (uc >= 32 && uc < 127) {
                        size_t len = strlen(vb->url_edit);
                        if (len < sizeof(vb->url_edit)-1) {
                            vb->url_edit[len]   = (char)uc;
                            vb->url_edit[len+1] = '\0';
                            vb->chrome_dirty = 1;
                        }
                    }
                }
            } else {
                dispatch_key(vb, &e.key);
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_WHEELUP ||
                e.button.button == SDL_BUTTON_WHEELDOWN) {
                if (e.button.y >= CHROME_H)
                    dispatch_scroll(vb, &e.button);
                break;
            }
            if (e.button.button != SDL_BUTTON_LEFT) break;

            {
                int sx = e.button.x, sy = e.button.y;
                int kb_y0 = WEBOS_SCREEN_H - KB_H;

                if (sy < CHROME_H) {
                    /* Chrome bar — dismiss URL editing if tapping outside URL box */
                    if (vb->url_editing &&
                        (sx < CHROME_URL_X || sx >= CHROME_URL_X + CHROME_URL_W))
                        dismiss_keyboard(vb);
                    chrome_handle_touch(vb, sx, sy);
                    vb->chrome_dirty = 1;
                } else {
                    /* WebKit area — dismiss URL editing if open */
                    if (vb->url_editing) dismiss_keyboard(vb);
                    vb->touch_was_scroll = 0;
                    vb->touch_active = 1;
                    vb->touch_x = sx;
                    vb->touch_y = sy - CHROME_H;
                    dispatch_touch(vb, vb->touch_x, vb->touch_y,
                                   wpe_input_touch_event_type_down);
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT && vb->touch_active) {
                vb->touch_active = 0;
                dispatch_touch(vb, e.button.x, e.button.y - CHROME_H,
                               wpe_input_touch_event_type_up);
            }
            break;

        case SDL_MOUSEMOTION:
            if (vb->touch_active) {
                int dx = e.motion.x - vb->touch_x;
                int dy = (e.motion.y - CHROME_H) - vb->touch_y;
                if (dx*dx + dy*dy > 16*16)
                    vb->touch_was_scroll = 1;
                vb->touch_x = e.motion.x;
                vb->touch_y = e.motion.y - CHROME_H;
                dispatch_touch(vb, vb->touch_x, vb->touch_y,
                               wpe_input_touch_event_type_motion);
            }
            break;

        case SDL_QUIT:
            exit(0);
        default:
            break;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * GLES2 COMPOSITOR
 * ════════════════════════════════════════════════════════════════════════════ */

static GLuint compile_blit_program(int bgra_swap)
{
    static const char *vert_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "    v_uv = a_uv;\n"
        "}\n";
    static const char *frag_rgba =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
    /* Cairo ARGB32 on LE ARM = bytes [B,G,R,A]; uploaded as GL_RGBA → swap r/b */
    static const char *frag_bgra =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main() {\n"
        "    vec4 t = texture2D(u_tex, v_uv);\n"
        "    gl_FragColor = vec4(t.b, t.g, t.r, t.a);\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vert_src, NULL);
    glCompileShader(vs);

    const char *fs_src = bgra_swap ? frag_bgra : frag_rgba;
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    fprintf(stderr, "[wpe-backend-sdl] %s shader: %s\n",
            bgra_swap ? "cairo(bgra)" : "webkit(rgba)", ok ? "OK" : "FAIL");
    return prog;
}

static GLuint make_tex(void)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void draw_quad(GLuint prog, GLuint tex,
                      float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1)
{
    float v[] = { x0,y0, u0,v1,  x1,y0, u1,v1,  x0,y1, u0,v0,  x1,y1, u1,v0 };
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), v+2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ── Composite all layers and swap — callable any time GL context is current ─ */

static void repaint(struct sdl_view_backend *vb)
{
    if (!vb->blit_ready) return;

    /* Lazily open the shared framebuffer the first time it's available */
    if (!vb->fb_pixels) {
        vb->fb_fd = open(PRISM_FB_PATH, O_RDONLY);
        if (vb->fb_fd >= 0) {
            vb->fb_pixels = mmap(NULL, vb->fb_size, PROT_READ, MAP_SHARED,
                                 vb->fb_fd, 0);
            if (vb->fb_pixels == MAP_FAILED) vb->fb_pixels = NULL;
        }
    }

    float chrome_top = 1.0f - 2.0f*(float)CHROME_H/WEBOS_SCREEN_H;

    /* ── WebKit — only re-upload texture when a new frame arrived ── */
    if (vb->fb_pixels) {
        if (vb->webkit_dirty) {
            glBindTexture(GL_TEXTURE_2D, vb->blit_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         WEBOS_SCREEN_W, WEBKIT_H, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, vb->fb_pixels);
            vb->webkit_dirty = 0;
        }
        draw_quad(vb->blit_prog, vb->blit_tex,
                  -1.f, -1.f, 1.f, chrome_top,
                   0.f,  0.f, 1.f, 1.f);
    }

    /* ── Chrome toolbar — only re-upload when dirty ── */
    if (vb->chrome_dirty) {
        chrome_render(vb);
        glBindTexture(GL_TEXTURE_2D, vb->chrome_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     WEBOS_SCREEN_W, CHROME_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, vb->chrome_pixels);
    }
    draw_quad(vb->cairo_prog, vb->chrome_tex,
              -1.f, chrome_top, 1.f, 1.f,
               0.f,  0.f,       1.f, 1.f);

    SDL_GL_SwapBuffers();
}

/* ── Combined SDL event pump + WebKit frame consumer ────────────────────────
 * Runs every 16 ms.  We always repaint — the shm pixels written by
 * WPEWebProcess are always current regardless of the signal socket state.
 * Gating repaint on socket signals caused freezes: when WebKit renders faster
 * than we drain, the socket fills, WPEWebProcess blocks on write(), no more
 * signals arrive, screen stops updating until a manual tap forced repaint().
 * Solution: drain the socket to keep dispatch_frame_displayed flowing, but
 * repaint unconditionally every tick. */

static gboolean on_sdl_poll(gpointer data)
{
    struct sdl_view_backend *vb = data;
    char buf[64];
    ssize_t n;

    /* Drain all pending frame signals — keep WebKit's render pipeline moving */
    while ((n = recv(vb->host_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        for (ssize_t i = 0; i < n; i++)
            wpe_view_backend_dispatch_frame_displayed(vb->wpe_backend);
        vb->webkit_dirty = 1;
    }

    pump_sdl_events(vb);
    repaint(vb);  /* always composite + swap, pixels in shm are always fresh */

    return G_SOURCE_CONTINUE;
}

/* GIO watch — wakes up the main loop immediately when a frame arrives so
 * on_sdl_poll fires without waiting out the full 16 ms timeout. */
static gboolean on_frame_signal(GIOChannel *channel, GIOCondition cond,
                                 gpointer data)
{
    (void)channel; (void)cond; (void)data;
    /* Actual work is done in on_sdl_poll; this just interrupts the poll. */
    return G_SOURCE_CONTINUE;
}

/* ════════════════════════════════════════════════════════════════════════════
 * VIEW BACKEND INTERFACE
 * ════════════════════════════════════════════════════════════════════════════ */

static void *view_backend_create(void *params, struct wpe_view_backend *wpe)
{
    (void)params;
    struct sdl_view_backend *vb = calloc(1, sizeof(*vb));
    if (!vb) return NULL;
    vb->wpe_backend  = wpe;
    vb->host_fd      = -1;
    vb->renderer_fd  = -1;
    vb->chrome_dirty = 1;
    vb->kb_dirty     = 1;
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) { free(vb); return NULL; }
    vb->host_fd     = fds[0];
    vb->renderer_fd = fds[1];
    g_vb = vb;
    return vb;
}

static void view_backend_destroy(void *data)
{
    struct sdl_view_backend *vb = data;
    if (!vb) return;
    if (vb->host_fd >= 0)     close(vb->host_fd);
    if (vb->renderer_fd >= 0) close(vb->renderer_fd);
    free(vb->kb_pixels);
    if (vb == g_vb) g_vb = NULL;
    free(vb);
}

static void view_backend_initialize(void *data)
{
    struct sdl_view_backend *vb = data;
    fprintf(stderr, "[wpe-backend-sdl] view_backend_initialize\n");

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE) < 0) {
            fprintf(stderr, "[wpe-backend-sdl] SDL_Init failed: %s\n", SDL_GetError());
            return;
        }
    }

    SDL_Surface *screen = SDL_SetVideoMode(WEBOS_SCREEN_W, WEBOS_SCREEN_H,
                                           0, SDL_OPENGL | SDL_NOFRAME);
    if (!screen)
        fprintf(stderr, "[wpe-backend-sdl] SDL_SetVideoMode failed: %s\n",
                SDL_GetError());
    SDL_EnableUNICODE(1);
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
    SDL_ShowCursor(SDL_DISABLE);

    /* Cache the EGL context SDL created so repaint() can re-assert it */
    vb->sdl_egl_dpy  = eglGetCurrentDisplay();
    vb->sdl_egl_surf = eglGetCurrentSurface(EGL_DRAW);
    vb->sdl_egl_ctx  = eglGetCurrentContext();
    fprintf(stderr, "[wpe-backend-sdl] SDL EGL: dpy=%p surf=%p ctx=%p\n",
            (void *)vb->sdl_egl_dpy, (void *)vb->sdl_egl_surf, (void *)vb->sdl_egl_ctx);

    vb->blit_prog  = compile_blit_program(0);
    vb->cairo_prog = compile_blit_program(1);
    vb->blit_tex   = make_tex();
    vb->chrome_tex = make_tex();
    vb->kb_tex     = make_tex();
    glViewport(0, 0, WEBOS_SCREEN_W, WEBOS_SCREEN_H);

    /* Keyboard pixel buffer */
    vb->kb_pixels = calloc(KB_H * WEBOS_SCREEN_W * 4, 1);
    kb_init(vb);
    kb_render(vb);

    /* Initial chrome */
    strncpy(vb->url_current, "Loading...", sizeof(vb->url_current)-1);
    chrome_render(vb);

    /* Shared framebuffer */
    vb->fb_size = (size_t)WEBOS_SCREEN_W * WEBKIT_H * 4;
    vb->fb_fd   = open(PRISM_FB_PATH, O_RDONLY);
    if (vb->fb_fd >= 0) {
        vb->fb_pixels = mmap(NULL, vb->fb_size, PROT_READ, MAP_SHARED,
                             vb->fb_fd, 0);
        if (vb->fb_pixels == MAP_FAILED) vb->fb_pixels = NULL;
    }

    vb->blit_ready = (vb->blit_prog && vb->cairo_prog);

    wpe_view_backend_dispatch_set_size(vb->wpe_backend, WEBOS_SCREEN_W, WEBKIT_H);
    wpe_view_backend_add_activity_state(vb->wpe_backend,
        wpe_view_activity_state_visible |
        wpe_view_activity_state_focused |
        wpe_view_activity_state_in_window);

    if (vb->host_fd >= 0) {
        GIOChannel *ch = g_io_channel_unix_new(vb->host_fd);
        g_io_add_watch(ch, G_IO_IN | G_IO_HUP, on_frame_signal, vb);
        g_io_channel_unref(ch);
    }
    g_timeout_add(16, on_sdl_poll, vb);
    fprintf(stderr, "[wpe-backend-sdl] ready: webkit=%dx%d kb_keys=%d\n",
            WEBOS_SCREEN_W, WEBKIT_H, vb->kb_ncells);
}

static int view_backend_get_renderer_host_fd(void *data)
{
    struct sdl_view_backend *vb = data;
    int fd = vb->renderer_fd;
    vb->renderer_fd = -1;
    return fd;
}

const struct wpe_view_backend_interface sdl_view_backend_interface = {
    .create               = view_backend_create,
    .destroy              = view_backend_destroy,
    .initialize           = view_backend_initialize,
    .get_renderer_host_fd = view_backend_get_renderer_host_fd,
};

/* ── Public chrome API (default visibility, resolved via dlsym) ─────────── */

__attribute__((visibility("default")))
void prism_set_chrome_callbacks(
    void (*navigate)(const char *, void *),
    void (*go_back)(void *),
    void (*go_fwd)(void *),
    void (*reload)(void *),
    void (*stop)(void *),
    void *user)
{
    if (!g_vb) return;
    g_vb->navigate_cb = navigate;
    g_vb->go_back_cb  = go_back;
    g_vb->go_fwd_cb   = go_fwd;
    g_vb->reload_cb   = reload;
    g_vb->stop_cb     = stop;
    g_vb->nav_user    = user;
}

__attribute__((visibility("default")))
void prism_chrome_set_url(const char *url)
{
    if (!g_vb || !url) return;
    if (!g_vb->url_editing) {
        strncpy(g_vb->url_current, url, sizeof(g_vb->url_current)-1);
        g_vb->url_current[sizeof(g_vb->url_current)-1] = '\0';
    }
    g_vb->chrome_dirty = 1;
}

__attribute__((visibility("default")))
void prism_chrome_set_nav_state(int can_back, int can_fwd, int loading)
{
    if (!g_vb) return;
    g_vb->can_go_back    = can_back;
    g_vb->can_go_forward = can_fwd;
    g_vb->loading        = loading;
    g_vb->chrome_dirty   = 1;
}
