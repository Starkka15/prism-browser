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
#include <GLES/glext.h>
/* GL_BGRA_EXT: PDK glextplatform.h may suppress this token.
 * 0x80E1 is the standard value from GL_EXT_bgra. */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
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
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <math.h>

static struct sdl_view_backend *g_vb = NULL;
struct sdl_view_backend *wpe_sdl_vb_get(void) { return g_vb; }

/* Forward declaration — defined later, called from pump_sdl_events */
static void resize_display(struct sdl_view_backend *vb, int w, int h);

/* ── URL resolution: scheme inference + search fallback ─────────────────────
 * Rules (in order):
 *  1. Has "://"          → use as-is
 *  2. Has "." or starts with "localhost" → prepend https://
 *  3. Otherwise          → DuckDuckGo search
 * Result written into buf (size buf_len).  Returns buf. */
static const char *resolve_url(const char *input, char *buf, size_t buf_len)
{
    if (!input || !input[0]) {
        strncpy(buf, "about:blank", buf_len - 1);
        buf[buf_len - 1] = '\0';
        return buf;
    }
    /* Already has a scheme */
    if (strstr(input, "://")) {
        strncpy(buf, input, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return buf;
    }
    /* Looks like a hostname (has a dot or is localhost) */
    if (strchr(input, '.') || strncmp(input, "localhost", 9) == 0) {
        snprintf(buf, buf_len, "https://%s", input);
        return buf;
    }
    /* Search query — URL-encode into DuckDuckGo Lite (much lighter page) */
    const char *prefix = "https://lite.duckduckgo.com/lite/?q=";
    size_t plen = strlen(prefix);
    strncpy(buf, prefix, buf_len - 1);
    buf[buf_len - 1] = '\0';
    size_t pos = plen;
    for (const char *p = input; *p && pos + 3 < buf_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ') {
            buf[pos++] = '+';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~') {
            buf[pos++] = (char)c;
        } else {
            /* percent-encode */
            static const char hex[] = "0123456789ABCDEF";
            buf[pos++] = '%';
            buf[pos++] = hex[c >> 4];
            buf[pos++] = hex[c & 0xf];
        }
    }
    buf[pos] = '\0';
    return buf;
}

/* ── Persistence paths ──────────────────────────────────────────────────── */
#define PRISM_HISTORY_FILE  "/media/internal/prism/history.txt"
#define PRISM_BMARK_FILE    "/media/internal/prism/bookmarks.txt"
#define PRISM_BMARK_HTML    "/media/internal/prism/bookmarks.html"

/* ── History ────────────────────────────────────────────────────────────── */

static void history_save(struct sdl_view_backend *vb)
{
    FILE *f = fopen(PRISM_HISTORY_FILE, "w");
    if (!f) return;
    for (int i = 0; i < vb->history_count; i++)
        fprintf(f, "%s\n", vb->history[i]);
    fclose(f);
}

static void history_load(struct sdl_view_backend *vb)
{
    vb->history_count = 0;
    FILE *f = fopen(PRISM_HISTORY_FILE, "r");
    if (!f) return;
    while (vb->history_count < HIST_MAX) {
        char *line = vb->history[vb->history_count];
        if (!fgets(line, 2048, f)) break;
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (line[0]) vb->history_count++;
    }
    fclose(f);
    fprintf(stderr, "[wpe-backend-sdl] history: %d entries\n", vb->history_count);
}

static void history_push(struct sdl_view_backend *vb, const char *url)
{
    if (!url || !url[0]) return;
    if (strncmp(url, "about:", 6) == 0) return;
    if (strncmp(url, "file://", 7) == 0) return;
    /* Move to front if duplicate */
    for (int i = 0; i < vb->history_count; i++) {
        if (strcmp(vb->history[i], url) == 0) {
            for (int j = i; j > 0; j--)
                memcpy(vb->history[j], vb->history[j-1], 2048);
            strncpy(vb->history[0], url, 2047);
            history_save(vb); return;
        }
    }
    /* Prepend new entry, dropping oldest if full */
    int top = vb->history_count < HIST_MAX - 1 ? vb->history_count : HIST_MAX - 1;
    for (int j = top; j > 0; j--)
        memcpy(vb->history[j], vb->history[j-1], 2048);
    strncpy(vb->history[0], url, 2047);
    vb->history[0][2047] = '\0';
    if (vb->history_count < HIST_MAX) vb->history_count++;
    history_save(vb);
}

/* ── Bookmarks ──────────────────────────────────────────────────────────── */

static void bookmark_write_page(struct sdl_view_backend *vb);  /* forward */

static void bookmark_save(struct sdl_view_backend *vb)
{
    FILE *f = fopen(PRISM_BMARK_FILE, "w");
    if (!f) return;
    for (int i = 0; i < vb->bookmark_count; i++)
        fprintf(f, "%s\n", vb->bookmarks[i]);
    fclose(f);
}

static void bookmark_load(struct sdl_view_backend *vb)
{
    vb->bookmark_count = 0;
    FILE *f = fopen(PRISM_BMARK_FILE, "r");
    if (f) {
        while (vb->bookmark_count < BMARK_MAX) {
            char *line = vb->bookmarks[vb->bookmark_count];
            if (!fgets(line, 2048, f)) break;
            char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
            if (line[0]) vb->bookmark_count++;
        }
        fclose(f);
    }
    bookmark_write_page(vb);
}

static int bookmark_check(struct sdl_view_backend *vb, const char *url)
{
    if (!url || !url[0]) return 0;
    for (int i = 0; i < vb->bookmark_count; i++)
        if (strcmp(vb->bookmarks[i], url) == 0) return 1;
    return 0;
}

static void bookmark_toggle(struct sdl_view_backend *vb)
{
    const char *url = vb->url_current;
    if (!url[0]) return;
    for (int i = 0; i < vb->bookmark_count; i++) {
        if (strcmp(vb->bookmarks[i], url) == 0) {
            /* Remove */
            for (int j = i; j < vb->bookmark_count - 1; j++)
                memcpy(vb->bookmarks[j], vb->bookmarks[j+1], 2048);
            vb->bookmark_count--;
            vb->is_bookmarked = 0;
            bookmark_save(vb);
            bookmark_write_page(vb);
            vb->chrome_dirty = 1;
            return;
        }
    }
    /* Add */
    if (vb->bookmark_count < BMARK_MAX) {
        strncpy(vb->bookmarks[vb->bookmark_count], url, 2047);
        vb->bookmarks[vb->bookmark_count][2047] = '\0';
        vb->bookmark_count++;
        vb->is_bookmarked = 1;
        bookmark_save(vb);
        bookmark_write_page(vb);
        vb->chrome_dirty = 1;
    }
}

static void html_escape(const char *s, char *buf, size_t sz)
{
    size_t n = 0;
    for (; *s && n + 8 < sz; s++) {
        if      (*s == '&') { memcpy(buf+n, "&amp;",  5); n+=5; }
        else if (*s == '<') { memcpy(buf+n, "&lt;",   4); n+=4; }
        else if (*s == '>') { memcpy(buf+n, "&gt;",   4); n+=4; }
        else if (*s == '"') { memcpy(buf+n, "&quot;", 6); n+=6; }
        else                { buf[n++] = *s; }
    }
    buf[n] = '\0';
}

static void bookmark_write_page(struct sdl_view_backend *vb)
{
    FILE *f = fopen(PRISM_BMARK_HTML, "w");
    if (!f) { fprintf(stderr, "[wpe-backend-sdl] cannot write bookmarks.html\n"); return; }
    fputs("<!DOCTYPE html>\n<html><head>"
          "<meta charset='utf-8'>"
          "<meta name='viewport' content='width=1024'>"
          "<title>Bookmarks</title>"
          "<style>*{margin:0;padding:0;box-sizing:border-box}"
          "body{background:#1a1a1a;color:#e0e0e0;font-family:sans-serif;padding:20px}"
          "h1{font-size:26px;color:#4a9fd5;margin-bottom:16px}"
          ".item{background:#2e2e2e;border-radius:8px;margin-bottom:8px;"
          "padding:14px 18px}"
          ".item a{color:#4a9fd5;text-decoration:none;font-size:16px;display:block;"
          "overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
          ".empty{color:#555;font-size:16px;margin-top:24px}"
          "</style></head><body>"
          "<h1>\342\230\205 Bookmarks</h1>\n", f);
    if (vb->bookmark_count == 0) {
        fputs("<p class='empty'>No bookmarks yet. "
              "Tap \342\230\205 in the toolbar to bookmark the current page.</p>\n", f);
    } else {
        char esc[4096];
        for (int i = 0; i < vb->bookmark_count; i++) {
            html_escape(vb->bookmarks[i], esc, sizeof(esc));
            fprintf(f, "<div class='item'><a href=\"%s\">%s</a></div>\n", esc, esc);
        }
    }
    fputs("</body></html>\n", f);
    fclose(f);
}

/* ── History dropdown ───────────────────────────────────────────────────── */

static int hist_drop_width(struct sdl_view_backend *vb)
{
    /* URL bar right edge = zoomm_x - 4, where zoomm_x is computed right-to-left */
    int star_x  = vb->screen_w - 4 - CHROME_STAR_W;
    int bkmk_x  = star_x  - 4 - CHROME_BMARK_NAV_W;
    int zoomp_x = bkmk_x  - 4 - CHROME_ZOOM_W;
    int zoomm_x = zoomp_x - 4 - CHROME_ZOOM_W;
    int url_bar_w = zoomm_x - CHROME_URL_X - 4;
    return url_bar_w < 80 ? 80 : url_bar_w;
}

static void hist_update(struct sdl_view_backend *vb)
{
    const char *q = vb->url_edit;
    int n = 0;
    for (int i = 0; i < vb->history_count && n < HIST_SHOW; i++) {
        if (!q[0] || strstr(vb->history[i], q)) {
            strncpy(vb->hist_rows[n], vb->history[i], 2047);
            vb->hist_rows[n][2047] = '\0';
            n++;
        }
    }
    vb->hist_nrows   = n;
    vb->hist_visible = (n > 0);
    vb->hist_dirty   = 1;
}

static void hist_render(struct sdl_view_backend *vb)
{
    if (!vb->hist_pixels || vb->hist_nrows <= 0) return;
    int dw = hist_drop_width(vb);
    int dh = vb->hist_nrows * HIST_ROW_H;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        vb->hist_pixels, CAIRO_FORMAT_ARGB32, dw, dh, dw * 4);
    cairo_t *cr = cairo_create(surf);

    cairo_set_source_rgba(cr, 0.09, 0.09, 0.09, 0.97);
    cairo_paint(cr);

    cairo_select_font_face(cr, "sans",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 16.0);

    for (int i = 0; i < vb->hist_nrows; i++) {
        double ry = (double)(i * HIST_ROW_H);
        if (i > 0) {
            cairo_set_source_rgba(cr, 0.22, 0.22, 0.22, 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 8.0, ry + 0.5);
            cairo_line_to(cr, dw - 8.0, ry + 0.5);
            cairo_stroke(cr);
        }
        cairo_save(cr);
        cairo_rectangle(cr, 8.0, ry + 4.0, (double)dw - 16.0, (double)HIST_ROW_H - 8.0);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.55, 0.75, 0.95);
        cairo_move_to(cr, 10.0, ry + HIST_ROW_H / 2.0 + 6.0);
        cairo_show_text(cr, vb->hist_rows[i]);
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    vb->hist_dirty = 0;
}

/* ── Download panel ─────────────────────────────────────────────────────── */

static int dl_active_count(struct sdl_view_backend *vb)
{
    int n = 0;
    for (int i = 0; i < DL_MAX_ENTRIES; i++)
        if (vb->dl_entries[i].status != DL_STATUS_FREE) n++;
    return n;
}

static int dl_panel_height(struct sdl_view_backend *vb)
{
    int n = dl_active_count(vb);
    return n > 0 ? DL_PANEL_HDR_H + n * DL_ROW_H : 0;
}

static void dl_render(struct sdl_view_backend *vb)
{
    int panel_h = dl_panel_height(vb);
    if (panel_h <= 0 || !vb->dl_pixels) return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        vb->dl_pixels, CAIRO_FORMAT_ARGB32, DL_PANEL_W, panel_h, DL_PANEL_W * 4);
    cairo_t *cr = cairo_create(surf);

    /* Panel background */
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.09, 0.97);
    cairo_paint(cr);

    /* Header: "Downloads" label */
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15.0);
    cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
    cairo_move_to(cr, 12.0, DL_PANEL_HDR_H - 9.0);
    cairo_show_text(cr, "Downloads");

    /* Header close button (×) */
    cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
    cairo_move_to(cr, DL_PANEL_W - 22.0, DL_PANEL_HDR_H - 9.0);
    cairo_show_text(cr, "\xc3\x97");   /* UTF-8 × */

    /* Header bottom divider */
    cairo_set_source_rgba(cr, 0.28, 0.28, 0.28, 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0.0, (double)DL_PANEL_HDR_H - 0.5);
    cairo_line_to(cr, (double)DL_PANEL_W, (double)DL_PANEL_HDR_H - 0.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    int row = 0;
    for (int i = 0; i < DL_MAX_ENTRIES; i++) {
        if (vb->dl_entries[i].status == DL_STATUS_FREE) continue;
        double ry = (double)(DL_PANEL_HDR_H + row * DL_ROW_H);
        int    st = vb->dl_entries[i].status;

        /* Row divider */
        if (row > 0) {
            cairo_set_source_rgba(cr, 0.20, 0.20, 0.20, 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 8.0, ry + 0.5);
            cairo_line_to(cr, (double)(DL_PANEL_W - 8), ry + 0.5);
            cairo_stroke(cr);
        }

        /* Filename (clipped) */
        cairo_set_font_size(cr, 14.0);
        cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
        cairo_save(cr);
        cairo_rectangle(cr, 8.0, ry + 4.0, (double)(DL_PANEL_W - 70), 20.0);
        cairo_clip(cr);
        cairo_move_to(cr, 10.0, ry + 18.0);
        cairo_show_text(cr, vb->dl_entries[i].filename[0]
                        ? vb->dl_entries[i].filename : "…");
        cairo_restore(cr);

        /* Status text */
        const char *stat_str;
        char pct_buf[16];
        if (st == DL_STATUS_DONE)        stat_str = "Done";
        else if (st == DL_STATUS_FAILED) stat_str = "Failed";
        else {
            snprintf(pct_buf, sizeof(pct_buf), "%d%%",
                     (int)(vb->dl_entries[i].progress * 100.0f + 0.5f));
            stat_str = pct_buf;
        }
        cairo_set_font_size(cr, 13.0);
        if (st == DL_STATUS_DONE)
            cairo_set_source_rgb(cr, 0.40, 0.80, 0.45);   /* green  */
        else if (st == DL_STATUS_FAILED)
            cairo_set_source_rgb(cr, 0.90, 0.35, 0.35);   /* red    */
        else
            cairo_set_source_rgb(cr, 0.55, 0.75, 0.95);   /* blue   */
        /* Right-align status: measure and offset */
        cairo_text_extents_t te;
        cairo_text_extents(cr, stat_str, &te);
        cairo_move_to(cr, (double)(DL_PANEL_W - 28) - te.width, ry + 18.0);
        cairo_show_text(cr, stat_str);

        /* Dismiss × button */
        cairo_set_font_size(cr, 16.0);
        cairo_set_source_rgb(cr, 0.50, 0.50, 0.50);
        cairo_move_to(cr, (double)(DL_PANEL_W - 22), ry + 20.0);
        cairo_show_text(cr, "\xc3\x97");

        /* Progress bar track */
        double bx = 10.0, bw = (double)(DL_PANEL_W - 20);
        double by = ry + 30.0, bh = 12.0;
        cairo_set_source_rgba(cr, 0.22, 0.22, 0.22, 1.0);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);

        /* Progress fill */
        double fill = st == DL_STATUS_DONE ? 1.0
                    : (double)vb->dl_entries[i].progress;
        if (fill < 0.0) fill = 0.0;
        if (fill > 1.0) fill = 1.0;
        if (st == DL_STATUS_DONE)
            cairo_set_source_rgba(cr, 0.29, 0.68, 0.31, 1.0);
        else if (st == DL_STATUS_FAILED)
            cairo_set_source_rgba(cr, 0.83, 0.18, 0.18, 1.0);
        else
            cairo_set_source_rgba(cr, 0.25, 0.56, 0.82, 1.0);
        if (fill > 0.005) {
            cairo_rectangle(cr, bx, by, bw * fill, bh);
            cairo_fill(cr);
        }

        row++;
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    vb->dl_dirty = 0;
}

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

/* Draw home icon: simple house (triangle roof + rectangle body) */
static void draw_home_icon(cairo_t *cr, double cx, double cy, double sz)
{
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    double hw = sz * 0.38;   /* half-width of house */
    double rh = sz * 0.26;   /* roof height */
    double bh = sz * 0.28;   /* body height */
    double ry = cy - bh/2.0 - rh + sz*0.04; /* roof apex y */
    /* Roof: triangle */
    cairo_new_path(cr);
    cairo_move_to(cr, cx,      ry);
    cairo_line_to(cr, cx - hw, ry + rh);
    cairo_line_to(cr, cx + hw, ry + rh);
    cairo_close_path(cr);
    cairo_fill(cr);
    /* Body: rectangle */
    double by = ry + rh;
    cairo_rectangle(cr, cx - hw*0.78, by, hw*1.56, bh);
    cairo_fill(cr);
    /* Door: small cutout (darker) */
    cairo_set_source_rgb(cr, 0.114, 0.114, 0.114);
    double dw = hw * 0.52, dh = bh * 0.62;
    cairo_rectangle(cr, cx - dw/2, by + bh - dh, dw, dh);
    cairo_fill(cr);
}

/* Draw zoom-out icon: small "A" with subscript "−" */
static void draw_zoom_out_icon(cairo_t *cr, double cx, double cy, double sz)
{
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, sz * 0.60);
    cairo_text_extents_t e;
    cairo_text_extents(cr, "A", &e);
    cairo_move_to(cr, cx - e.x_advance * 0.6, cy + e.height * 0.35);
    cairo_show_text(cr, "A");
    cairo_set_font_size(cr, sz * 0.42);
    cairo_text_extents(cr, "-", &e);
    cairo_move_to(cr, cx + e.x_advance * 0.1, cy - e.height * 0.5);
    cairo_show_text(cr, "-");
}

/* Draw zoom-in icon: large "A" with superscript "+" */
static void draw_zoom_in_icon(cairo_t *cr, double cx, double cy, double sz)
{
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, sz * 0.72);
    cairo_text_extents_t e;
    cairo_text_extents(cr, "A", &e);
    cairo_move_to(cr, cx - e.x_advance * 0.60, cy + e.height * 0.35);
    cairo_show_text(cr, "A");
    cairo_set_font_size(cr, sz * 0.42);
    cairo_text_extents(cr, "+", &e);
    cairo_move_to(cr, cx + e.x_advance * 0.05, cy - e.height * 0.50);
    cairo_show_text(cr, "+");
}

/* Draw bookmarks-list icon: three horizontal lines */
static void draw_bmark_nav_icon(cairo_t *cr, double cx, double cy, double sz)
{
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_set_line_width(cr, sz * 0.10);
    double w = sz * 0.52;
    double gap = sz * 0.16;
    for (int i = -1; i <= 1; i++) {
        double y = cy + i * gap;
        cairo_move_to(cr, cx - w/2, y);
        cairo_line_to(cr, cx + w/2, y);
        cairo_stroke(cr);
    }
}

static void draw_star_icon(cairo_t *cr, double cx, double cy, double sz, int filled)
{
    double r_out = sz * 0.44, r_in = sz * 0.18;
    double pi = M_PI;
    cairo_new_path(cr);
    for (int i = 0; i < 5; i++) {
        double ao = -pi/2.0 + i * 2.0*pi/5.0;
        double ai = ao + pi/5.0;
        if (i == 0) cairo_move_to(cr, cx + r_out*cos(ao), cy + r_out*sin(ao));
        else        cairo_line_to(cr, cx + r_out*cos(ao), cy + r_out*sin(ao));
        cairo_line_to(cr, cx + r_in*cos(ai), cy + r_in*sin(ai));
    }
    cairo_close_path(cr);
    if (filled) {
        cairo_set_source_rgb(cr, 1.0, 0.85, 0.10);
        cairo_fill(cr);
    } else {
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }
}

/* Compute right-side button x positions (right-to-left) */
static void chrome_right_buttons(struct sdl_view_backend *vb,
    int *star_x, int *bkmk_x, int *zoomp_x, int *zoomm_x, int *url_bar_w)
{
    *star_x   = vb->screen_w - 4 - CHROME_STAR_W;
    *bkmk_x   = *star_x  - 4 - CHROME_BMARK_NAV_W;
    *zoomp_x  = *bkmk_x  - 4 - CHROME_ZOOM_W;
    *zoomm_x  = *zoomp_x - 4 - CHROME_ZOOM_W;
    *url_bar_w = *zoomm_x - CHROME_URL_X - 4;
    if (*url_bar_w < 80) *url_bar_w = 80;   /* safety floor */
}

static void chrome_render(struct sdl_view_backend *vb)
{
    int star_x, bkmk_x, zoomp_x, zoomm_x, url_bar_w;
    chrome_right_buttons(vb, &star_x, &bkmk_x, &zoomp_x, &zoomm_x, &url_bar_w);
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        vb->chrome_pixels, CAIRO_FORMAT_ARGB32,
        vb->screen_w, CHROME_H, vb->screen_w * 4);
    cairo_t *cr = cairo_create(surf);

    cairo_set_source_rgb(cr, 0.114, 0.114, 0.114);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, CHROME_H - 0.5);
    cairo_line_to(cr, vb->screen_w, CHROME_H - 0.5);
    cairo_stroke(cr);

    double btn_cx[4] = {
        CHROME_BACK_X   + CHROME_BTN_W/2.0,
        CHROME_FWD_X    + CHROME_BTN_W/2.0,
        CHROME_RELOAD_X + CHROME_BTN_W/2.0,
        CHROME_HOME_X   + CHROME_BTN_W/2.0,
    };
    double btn_cy = CHROME_BTN_Y + CHROME_BTN_H/2.0;

    for (int i = 0; i < 4; i++) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.04);
        rounded_rect(cr, btn_cx[i]-CHROME_BTN_W/2.0, CHROME_BTN_Y,
                     CHROME_BTN_W, CHROME_BTN_H, 6.0);
        cairo_fill(cr);
    }

    draw_back_icon  (cr, btn_cx[0], btn_cy, 20.0, vb->can_go_back);
    draw_fwd_icon   (cr, btn_cx[1], btn_cy, 20.0, vb->can_go_forward);
    draw_reload_icon(cr, btn_cx[2], btn_cy, 20.0, vb->loading);
    draw_home_icon  (cr, btn_cx[3], btn_cy, 20.0);

    /* Zoom-out button */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.04);
    rounded_rect(cr, (double)zoomm_x, CHROME_BTN_Y, CHROME_ZOOM_W, CHROME_BTN_H, 6.0);
    cairo_fill(cr);
    draw_zoom_out_icon(cr, zoomm_x + CHROME_ZOOM_W/2.0, btn_cy, 20.0);

    /* Zoom-in button */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.04);
    rounded_rect(cr, (double)zoomp_x, CHROME_BTN_Y, CHROME_ZOOM_W, CHROME_BTN_H, 6.0);
    cairo_fill(cr);
    draw_zoom_in_icon(cr, zoomp_x + CHROME_ZOOM_W/2.0, btn_cy, 20.0);

    /* Bookmarks-nav button */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.04);
    rounded_rect(cr, (double)bkmk_x, CHROME_BTN_Y, CHROME_BMARK_NAV_W, CHROME_BTN_H, 6.0);
    cairo_fill(cr);
    draw_bmark_nav_icon(cr, bkmk_x + CHROME_BMARK_NAV_W/2.0, btn_cy, 20.0);

    /* Star (bookmark toggle) button */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.04);
    rounded_rect(cr, (double)star_x, CHROME_BTN_Y, CHROME_STAR_W, CHROME_BTN_H, 6.0);
    cairo_fill(cr);
    draw_star_icon(cr, star_x + CHROME_STAR_W/2.0, btn_cy, 20.0, vb->is_bookmarked);

    rounded_rect(cr, CHROME_URL_X, CHROME_URL_Y,
                 url_bar_w, CHROME_URL_H, 6.0);
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
                 url_bar_w-4, CHROME_URL_H-4, 5.0);
    cairo_clip(cr);

    /* Progress bar — blue fill behind URL text while loading */
    if (vb->loading && vb->load_progress > 0.0f) {
        float frac = vb->load_progress < 1.0f ? vb->load_progress : 1.0f;
        double fill_w = (url_bar_w - 4) * frac;
        cairo_set_source_rgba(cr, 0.18, 0.48, 0.82, 0.45);
        rounded_rect(cr, CHROME_URL_X+2, CHROME_URL_Y+2,
                     fill_w, CHROME_URL_H-4, 5.0);
        cairo_fill(cr);
    }

    /* Favicon (20×20) left of URL text */
    double text_x = CHROME_URL_X + 12.0;
    if (vb->favicon_surf) {
        cairo_surface_t *fav = (cairo_surface_t *)vb->favicon_surf;
        cairo_set_source_surface(cr, fav,
                                 CHROME_URL_X + 6.0,
                                 CHROME_URL_Y + (CHROME_URL_H - 20) / 2.0);
        cairo_paint(cr);
        text_x = CHROME_URL_X + 32.0;
    }

    cairo_move_to(cr, text_x, CHROME_URL_Y + CHROME_URL_H/2.0 + 7.0);
    if (display[0]) {
        cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
        cairo_show_text(cr, display);
    } else {
        /* Placeholder — dimmed, shown when not editing and no URL loaded */
        cairo_set_source_rgba(cr, 0.50, 0.50, 0.50, 1.0);
        cairo_show_text(cr, "Enter URL or search terms");
    }

    if (vb->url_editing) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, display, &ext);
        double cx2 = text_x + ext.x_advance + 2.0;
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
    int star_x, bkmk_x, zoomp_x, zoomm_x, url_bar_w;
    chrome_right_buttons(vb, &star_x, &bkmk_x, &zoomp_x, &zoomm_x, &url_bar_w);
    int url_right = zoomm_x - 4;

    if (x >= CHROME_BACK_X && x < CHROME_BACK_X + CHROME_BTN_W) {
        if (vb->can_go_back && vb->go_back_cb)
            vb->go_back_cb(vb->nav_user);
    } else if (x >= CHROME_FWD_X && x < CHROME_FWD_X + CHROME_BTN_W) {
        if (vb->can_go_forward && vb->go_fwd_cb)
            vb->go_fwd_cb(vb->nav_user);
    } else if (x >= CHROME_RELOAD_X && x < CHROME_RELOAD_X + CHROME_BTN_W) {
        if (vb->loading && vb->stop_cb)         vb->stop_cb(vb->nav_user);
        else if (!vb->loading && vb->reload_cb)  vb->reload_cb(vb->nav_user);
    } else if (x >= CHROME_HOME_X && x < CHROME_HOME_X + CHROME_BTN_W) {
        if (vb->home_cb) vb->home_cb(vb->nav_user);
    } else if (x >= zoomm_x && x < zoomm_x + CHROME_ZOOM_W) {
        if (vb->zoom_out_cb) vb->zoom_out_cb(vb->nav_user);
    } else if (x >= zoomp_x && x < zoomp_x + CHROME_ZOOM_W) {
        if (vb->zoom_in_cb)  vb->zoom_in_cb(vb->nav_user);
    } else if (x >= bkmk_x && x < bkmk_x + CHROME_BMARK_NAV_W) {
        if (vb->navigate_cb)
            vb->navigate_cb("file:///media/internal/prism/bookmarks.html",
                            vb->nav_user);
    } else if (x >= star_x && x < star_x + CHROME_STAR_W) {
        bookmark_toggle(vb);
    } else if (x >= CHROME_URL_X && x < url_right) {
        if (!vb->url_editing) {
            vb->url_editing = 1;
            vb->url_edit[0] = '\0';
            PDL_SetKeyboardState(PDL_TRUE);
            hist_update(vb);
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
    const int avail_w = vb->screen_w - 2*pad_x;

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
        vb->screen_w, KB_H, vb->screen_w * 4);
    cairo_t *cr = cairo_create(surf);

    /* Background */
    cairo_set_source_rgb(cr, 0.137, 0.137, 0.137);
    cairo_paint(cr);

    /* Top separator */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, 0.5);
    cairo_line_to(cr, vb->screen_w, 0.5);
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
    int kb_y0 = vb->screen_h - KB_H;
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
        char buf[2200];
        const char *url = resolve_url(vb->url_edit, buf, sizeof(buf));
        if (vb->navigate_cb && url[0] && strcmp(url, "about:blank") != 0)
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
        if (vb->blur_input_cb) vb->blur_input_cb(vb->nav_user);
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
    /* Blur any focused web input first — otherwise WebKit's notify_focus_in
     * fires immediately after PDL_SetKeyboardState(FALSE) and re-opens it. */
    if (vb->pdl_kb_visible && vb->blur_input_cb)
        vb->blur_input_cb(vb->nav_user);

    if (vb->url_editing) {
        vb->url_editing  = 0;
        vb->chrome_dirty = 1;
        PDL_SetKeyboardState(PDL_FALSE);
    }
    if (vb->hist_visible) {
        vb->hist_visible = 0;
        vb->hist_nrows   = 0;
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
                    char buf[2200];
                    const char *url = resolve_url(vb->url_edit, buf, sizeof(buf));
                    if (vb->navigate_cb && url[0] && strcmp(url, "about:blank") != 0)
                        vb->navigate_cb(url, vb->nav_user);
                    dismiss_keyboard(vb);
                } else if (sym == SDLK_ESCAPE) {
                    dismiss_keyboard(vb);
                } else if (sym == SDLK_BACKSPACE) {
                    size_t len = strlen(vb->url_edit);
                    if (len > 0) vb->url_edit[len-1] = '\0';
                    hist_update(vb);
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
                            hist_update(vb);
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
                int hist_bottom = CHROME_H + vb->hist_nrows * HIST_ROW_H;
                int dw = hist_drop_width(vb);

                if (sy < CHROME_H) {
                    /* Chrome bar — dismiss keyboard if tapping outside URL bar */
                    if (vb->url_editing) {
                        int _s, _b, _zp, _zm, _ubw;
                        chrome_right_buttons(vb, &_s, &_b, &_zp, &_zm, &_ubw);
                        int _url_right = _zm - 4;
                        if (sx < CHROME_URL_X || sx >= _url_right)
                            dismiss_keyboard(vb);
                    }
                    chrome_handle_touch(vb, sx, sy);
                    vb->chrome_dirty = 1;
                } else if (vb->hist_visible &&
                           sy < hist_bottom &&
                           sx >= CHROME_URL_X &&
                           sx < CHROME_URL_X + dw) {
                    /* History dropdown tap */
                    int row = (sy - CHROME_H) / HIST_ROW_H;
                    if (row >= 0 && row < vb->hist_nrows) {
                        const char *url = vb->hist_rows[row];
                        if (vb->navigate_cb)
                            vb->navigate_cb(url, vb->nav_user);
                    }
                    vb->hist_visible = 0;
                    vb->hist_nrows   = 0;
                    dismiss_keyboard(vb);
                } else if (vb->dl_visible &&
                           sx >= vb->screen_w - DL_PANEL_W &&
                           sy >= CHROME_H &&
                           sy < CHROME_H + dl_panel_height(vb)) {
                    /* Download panel tap */
                    int rel_y = sy - CHROME_H;
                    if (rel_y < DL_PANEL_HDR_H) {
                        /* Header × closes panel */
                        if (sx >= vb->screen_w - 32) {
                            vb->dl_visible = 0;
                            free(vb->dl_pixels); vb->dl_pixels = NULL;
                        }
                    } else {
                        /* Row × dismisses that entry */
                        if (sx >= vb->screen_w - 36) {
                            int row = (rel_y - DL_PANEL_HDR_H) / DL_ROW_H;
                            int n = 0;
                            for (int i = 0; i < DL_MAX_ENTRIES; i++) {
                                if (vb->dl_entries[i].status == DL_STATUS_FREE) continue;
                                if (n == row) {
                                    vb->dl_entries[i].status = DL_STATUS_FREE;
                                    vb->dl_dirty = 1;
                                    break;
                                }
                                n++;
                            }
                        }
                    }
                } else {
                    /* WebKit area — dismiss dropdown + keyboard */
                    if (vb->hist_visible) {
                        vb->hist_visible = 0;
                        vb->hist_nrows   = 0;
                    }
                    if (vb->url_editing) dismiss_keyboard(vb);
                    /* If PDL keyboard is up and user taps web content,
                     * blur the active element so keyboard actually dismisses. */
                    if (vb->pdl_kb_visible && vb->blur_input_cb)
                        vb->blur_input_cb(vb->nav_user);
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

        case SDL_VIDEORESIZE:
            /* Orientation changed — re-create SDL surface and resize everything */
            {
                SDL_Surface *ns = SDL_SetVideoMode(e.resize.w, e.resize.h,
                                                   0, SDL_OPENGL | SDL_NOFRAME | SDL_RESIZABLE);
                if (ns) {
                    EGLSurface new_surf = eglGetCurrentSurface(EGL_DRAW);
                    if (new_surf != EGL_NO_SURFACE) vb->sdl_egl_surf = new_surf;
                    EGLContext new_ctx = eglGetCurrentContext();
                    if (new_ctx != EGL_NO_CONTEXT) vb->sdl_egl_ctx = new_ctx;
                    eglMakeCurrent(vb->sdl_egl_dpy, vb->sdl_egl_surf,
                                   vb->sdl_egl_surf, vb->sdl_egl_ctx);
                    resize_display(vb, ns->w, ns->h);
                } else {
                    fprintf(stderr, "[wpe-backend-sdl] SDL_VIDEORESIZE SetVideoMode failed: %s\n",
                            SDL_GetError());
                }
            }
            break;

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

    float chrome_top = 1.0f - 2.0f*(float)CHROME_H/vb->screen_h;

    /* ── WebKit layer ────────────────────────────────────────────────────── */
    if (vb->qcom_ready) {
        /*
         * Zero-copy path: blit_tex is backed by the QCOM shared GPU image.
         * WPEWebProcess wrote the latest frame into it via glCopyTexSubImage2D.
         * No CPU upload needed — just draw. Re-bind the EGLImage each time to
         * ensure the driver exposes the latest content to this GL context.
         */
        glBindTexture(GL_TEXTURE_2D, vb->blit_tex);
        vb->pfn_ImageTargetTex(GL_TEXTURE_2D,
                               (GLeglImageOES)vb->qcom_shared_image);
        draw_quad(vb->blit_prog, vb->blit_tex,
                  -1.f, -1.f, 1.f, chrome_top,
                   0.f,  0.f, 1.f, 1.f);
    } else if (vb->fb_pixels) {
        /* Fallback: CPU mmap path — upload only when WP signals a new frame.
         * 'F' signals now arrive via /tmp/prism-fb-frame.fifo (on_sdl_poll)
         * which works for all WebProcesses regardless of IPC fd validity.
         *
         * Use GL_BGRA_EXT: PowerVR lock-surface returns pixels in BGRA order.
         * Use glTexSubImage2D: no storage realloc per frame (faster). */
        if (vb->webkit_dirty) {
            glBindTexture(GL_TEXTURE_2D, vb->blit_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                            0, 0, vb->screen_w, vb->webkit_h,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, vb->fb_pixels);
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
                     vb->screen_w, CHROME_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, vb->chrome_pixels);
        vb->chrome_dirty = 0;
    }
    draw_quad(vb->cairo_prog, vb->chrome_tex,
              -1.f, chrome_top, 1.f, 1.f,
               0.f,  0.f,       1.f, 1.f);

    /* ── History dropdown (overlays top of WebKit area) ── */
    if (vb->hist_visible && vb->hist_nrows > 0) {
        int dw = hist_drop_width(vb);
        int dh = vb->hist_nrows * HIST_ROW_H;
        if (vb->hist_dirty) {
            /* (Re)allocate pixel buffer if size changed */
            size_t need = (size_t)dw * dh * 4;
            if (!vb->hist_pixels) {
                vb->hist_pixels = malloc(need);
                glBindTexture(GL_TEXTURE_2D, vb->hist_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dw, dh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            }
            if (vb->hist_pixels) {
                hist_render(vb);
                glBindTexture(GL_TEXTURE_2D, vb->hist_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dw, dh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, vb->hist_pixels);
            }
        }
        if (vb->hist_pixels) {
            float x0 = -1.0f + 2.0f * CHROME_URL_X / vb->screen_w;
            float x1 = x0    + 2.0f * dw            / vb->screen_w;
            float y1 = chrome_top;
            float y0 = y1 - 2.0f * dh / vb->screen_h;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw_quad(vb->cairo_prog, vb->hist_tex,
                      x0, y0, x1, y1,
                      0.f, 0.f, 1.f, 1.f);
            glDisable(GL_BLEND);
        }
    }

    /* ── Download panel (right-side overlay) ── */
    if (vb->dl_visible) {
        int panel_h = dl_panel_height(vb);

        /* Auto-hide: once all entries are done/failed, wait 4 s then clear */
        if (panel_h > 0) {
            int has_active = 0;
            for (int i = 0; i < DL_MAX_ENTRIES; i++)
                if (vb->dl_entries[i].status == DL_STATUS_ACTIVE) { has_active = 1; break; }
            if (!has_active) {
                if (!vb->dl_done_at_ms) vb->dl_done_at_ms = now_ms();
                else if (now_ms() - vb->dl_done_at_ms > 4000) {
                    for (int i = 0; i < DL_MAX_ENTRIES; i++)
                        vb->dl_entries[i].status = DL_STATUS_FREE;
                    vb->dl_visible    = 0;
                    vb->dl_done_at_ms = 0;
                    free(vb->dl_pixels); vb->dl_pixels = NULL;
                    panel_h = 0;
                }
            } else {
                vb->dl_done_at_ms = 0;
            }
        } else {
            vb->dl_visible = 0;
        }

        if (vb->dl_visible && panel_h > 0) {
            if (vb->dl_dirty || !vb->dl_pixels || vb->dl_panel_h != panel_h) {
                free(vb->dl_pixels);
                vb->dl_pixels  = malloc((size_t)DL_PANEL_W * panel_h * 4);
                vb->dl_panel_h = panel_h;
                if (vb->dl_pixels) {
                    dl_render(vb);
                    glBindTexture(GL_TEXTURE_2D, vb->dl_tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                 DL_PANEL_W, panel_h, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, vb->dl_pixels);
                }
            }
            if (vb->dl_pixels) {
                float px0 = 1.0f - 2.0f * DL_PANEL_W / vb->screen_w;
                float py1 = chrome_top;
                float py0 = py1 - 2.0f * panel_h / vb->screen_h;
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                draw_quad(vb->cairo_prog, vb->dl_tex,
                          px0, py0, 1.0f, py1,
                          0.f,  0.f, 1.0f, 1.0f);
                glDisable(GL_BLEND);
            }
        }
    }

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

    /* ── QCOM init packet from WPEWebProcess ────────────────────────────────
     * WP sends exactly one 8-byte packet {share_id, dpy} from ensure_pbuffer
     * (first frame_will_render), BEFORE any 'F' frame signals.  Peek to check
     * if the packet is ready without consuming 'F' bytes that may follow. */
    if (!vb->qcom_received) {
        EGLint pkt[2] = { 0, 0 };
        n = recv(vb->host_fd, pkt, sizeof(pkt), MSG_PEEK | MSG_DONTWAIT);
        if (n == (ssize_t)sizeof(pkt)) {
            /* Consume the 8 bytes */
            recv(vb->host_fd, pkt, sizeof(pkt), MSG_DONTWAIT);
            vb->qcom_received = 1;
            fprintf(stderr,
                    "[wpe-backend-sdl] QCOM init from WP:"
                    " share_id=0x%x dpy=0x%x\n",
                    (unsigned)pkt[0], (unsigned)pkt[1]);

            if (pkt[0] != 0 && vb->pfn_CreateImage && vb->pfn_ImageTargetTex) {
                /* Import the shared image WP created on its own display.
                 * Use our own sdl_egl_dpy — it IS valid in this process. */
                const EGLint import_attrs[] = {
                    EGL_SHARED_IMAGE_ID_QCOM, pkt[0],
                    EGL_NONE
                };
                eglGetError(); /* clear any pending EGL error */
                EGLImageKHR img = vb->pfn_CreateImage(
                    vb->sdl_egl_dpy, EGL_NO_CONTEXT,
                    (EGLenum)EGL_SHARED_IMAGE_QCOM,
                    (EGLClientBuffer)NULL,
                    import_attrs);
                fprintf(stderr,
                        "[wpe-backend-sdl] eglCreateImageKHR(0x3120) import"
                        " -> %p (egl_err=0x%x)\n",
                        (void *)img, (unsigned)eglGetError());
                if (img != EGL_NO_IMAGE_KHR) {
                    vb->qcom_shared_image = img;
                    glBindTexture(GL_TEXTURE_2D, vb->blit_tex);
                    glGetError(); /* clear */
                    vb->pfn_ImageTargetTex(GL_TEXTURE_2D,
                                           (GLeglImageOES)img);
                    GLenum gl_err = glGetError();
                    if (gl_err == GL_NO_ERROR) {
                        vb->qcom_ready = 1;
                        fprintf(stderr,
                                "[wpe-backend-sdl] QCOM import OK"
                                " — zero-copy active\n");
                    } else {
                        fprintf(stderr,
                                "[wpe-backend-sdl] QCOM import:"
                                " glEGLImageTargetTexture2DOES failed:"
                                " 0x%x\n", (unsigned)gl_err);
                        vb->pfn_DestroyImage(vb->sdl_egl_dpy, img);
                        vb->qcom_shared_image = EGL_NO_IMAGE_KHR;
                    }
                } else {
                    fprintf(stderr,
                            "[wpe-backend-sdl] QCOM import:"
                            " eglCreateImageKHR failed:"
                            " 0x%x\n", (unsigned)eglGetError());
                }
            } else if (pkt[0] == 0) {
                fprintf(stderr,
                        "[wpe-backend-sdl] WP QCOM failed"
                        " — staying on prism-fb fallback\n");
            }

            /* Send ACK to WP: 1 = QCOM import OK, 0 = use prism-fb.
             * WP waits for this (non-blocking in frame_rendered) before
             * enabling the QCOM GPU blit path. */
            {
                EGLint ack = vb->qcom_ready ? 1 : 0;
                ssize_t sent = write(vb->host_fd, &ack, sizeof(ack));
                fprintf(stderr,
                        "[wpe-backend-sdl] QCOM ACK sent to WP: %d"
                        " (%zd bytes)\n", ack, sent);
            }
        } else if (n > 0) {
            /* Partial read (< 8 bytes) — shouldn't happen on socketpair.
             * Don't consume; retry next tick. */
        } else {
            /* No data yet — WP hasn't rendered first frame.  Fall through
             * to pump + repaint so UI stays responsive on prism-fb. */
        }
    }

    /* Drain all pending frame signals — keep WebKit's render pipeline moving */
    if (vb->qcom_received) {
        while ((n = recv(vb->host_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            for (ssize_t i = 0; i < n; i++)
                wpe_view_backend_dispatch_frame_displayed(vb->wpe_backend);
            vb->webkit_dirty = 1;
        }
    }

    /* Drain 'F' frame signals from FIFO — covers ALL WebProcesses regardless
     * of which IPC socketpair is currently live.  Each 'F' byte = one frame. */
    if (vb->frame_fifo_fd >= 0) {
        while ((n = read(vb->frame_fifo_fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++)
                wpe_view_backend_dispatch_frame_displayed(vb->wpe_backend);
            vb->webkit_dirty = 1;
        }
    }

    pump_sdl_events(vb);
    /* Only repaint when something actually changed — avoids 60fps eglSwapBuffers
     * cost (~15-20% CPU on Cortex-A9) while screen is static. */
    if (vb->webkit_dirty || vb->chrome_dirty || vb->kb_dirty)
        repaint(vb);

    return G_SOURCE_CONTINUE;
}

/* ── Dynamic resize ──────────────────────────────────────────────────────────
 * Called at init (with actual SDL surface dims) and on SDL_VIDEORESIZE.
 * Updates all size-dependent state: runtime fields, textures, prism-fb,
 * cairo pixel buffers, GL viewport, and WebKit view size. */
static void resize_display(struct sdl_view_backend *vb, int w, int h)
{
    if (w == vb->screen_w && h == vb->screen_h) return;

    fprintf(stderr, "[wpe-backend-sdl] resize: %dx%d → %dx%d\n",
            vb->screen_w, vb->screen_h, w, h);

    vb->screen_w = w;
    vb->screen_h = h;
    vb->webkit_h = h - CHROME_H;

    /* Chrome pixel buffer */
    free(vb->chrome_pixels);
    vb->chrome_pixels = calloc((size_t)CHROME_H * w * 4, 1);
    vb->chrome_dirty  = 1;

    /* Keyboard pixel buffer */
    free(vb->kb_pixels);
    vb->kb_pixels = calloc((size_t)KB_H * w * 4, 1);
    kb_init(vb);
    vb->kb_dirty  = 1;

    /* History dropdown pixel buffer — free; repaint() reallocates at new width */
    free(vb->hist_pixels);
    vb->hist_pixels = NULL;
    vb->hist_dirty  = 1;

    /* Download panel pixel buffer — free; repaint() reallocates */
    free(vb->dl_pixels);
    vb->dl_pixels  = NULL;
    vb->dl_panel_h = 0;
    vb->dl_dirty   = 1;

    /* GL viewport */
    glViewport(0, 0, w, h);

    /* Re-allocate texture storage for new dimensions */
    glBindTexture(GL_TEXTURE_2D, vb->blit_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, vb->webkit_h, 0,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, vb->chrome_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, CHROME_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    /* prism-fb: resize the shared framebuffer */
    size_t new_fb_size = (size_t)w * vb->webkit_h * 4;
    if (vb->fb_pixels) { munmap(vb->fb_pixels, vb->fb_size); vb->fb_pixels = NULL; }
    vb->fb_size = new_fb_size;
    {
        int cfd = open(PRISM_FB_PATH, O_RDWR);
        if (cfd >= 0) {
            ftruncate(cfd, (off_t)new_fb_size);
            close(cfd);
        }
    }
    if (vb->fb_fd >= 0) {
        vb->fb_pixels = mmap(NULL, vb->fb_size, PROT_READ, MAP_SHARED,
                             vb->fb_fd, 0);
        if (vb->fb_pixels == MAP_FAILED) vb->fb_pixels = NULL;
    }

    /* Notify WebKit of new content area size */
    wpe_view_backend_dispatch_set_size(vb->wpe_backend, w, vb->webkit_h);
    vb->webkit_dirty = 1;

    fprintf(stderr, "[wpe-backend-sdl] resized: webkit=%dx%d\n", w, vb->webkit_h);
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
    vb->frame_fifo_fd = -1;
    vb->chrome_dirty  = 1;
    vb->kb_dirty      = 1;
    vb->webkit_dirty  = 1;  /* force initial paint before first WP frame */
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
    if (vb->qcom_shared_image != EGL_NO_IMAGE_KHR && vb->pfn_DestroyImage)
        vb->pfn_DestroyImage(vb->sdl_egl_dpy, vb->qcom_shared_image);
    if (vb->fb_pixels)
        munmap(vb->fb_pixels, vb->fb_size);
    if (vb->fb_fd >= 0)
        close(vb->fb_fd);
    if (vb->host_fd >= 0)     close(vb->host_fd);
    if (vb->renderer_fd >= 0) close(vb->renderer_fd);
    if (vb->frame_fifo_fd >= 0) close(vb->frame_fifo_fd);
    unlink("/tmp/prism-fb-frame.fifo");
    free(vb->chrome_pixels);
    free(vb->kb_pixels);
    free(vb->hist_pixels);
    free(vb->dl_pixels);
    if (vb->favicon_surf)
        cairo_surface_destroy((cairo_surface_t *)vb->favicon_surf);
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

    /* Request fullscreen surface; read back actual dims from driver */
    SDL_Surface *screen = SDL_SetVideoMode(0, 0, 0,
                                           SDL_OPENGL | SDL_NOFRAME | SDL_RESIZABLE);
    if (!screen) {
        fprintf(stderr, "[wpe-backend-sdl] SDL_SetVideoMode failed: %s\n",
                SDL_GetError());
        return;
    }
    SDL_EnableUNICODE(1);
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
    SDL_ShowCursor(SDL_DISABLE);

    /* Set runtime dimensions from what the driver actually gave us */
    vb->screen_w = screen->w > 0 ? screen->w : 1024;
    vb->screen_h = screen->h > 0 ? screen->h : 768;
    vb->webkit_h = vb->screen_h - CHROME_H;
    fprintf(stderr, "[wpe-backend-sdl] screen: %dx%d webkit_h=%d\n",
            vb->screen_w, vb->screen_h, vb->webkit_h);

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

    /* Allocate chrome + kb pixel buffers, set viewport, alloc textures */
    vb->chrome_pixels = calloc((size_t)CHROME_H * vb->screen_w * 4, 1);
    vb->kb_pixels     = calloc((size_t)KB_H      * vb->screen_w * 4, 1);
    glViewport(0, 0, vb->screen_w, vb->screen_h);
    glBindTexture(GL_TEXTURE_2D, vb->blit_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vb->screen_w, vb->webkit_h, 0,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);

    kb_init(vb);
    kb_render(vb);

    history_load(vb);
    bookmark_load(vb);
    vb->hist_tex = make_tex();
    vb->dl_tex   = make_tex();
    for (int i = 0; i < DL_MAX_ENTRIES; i++)
        vb->dl_entries[i].status = DL_STATUS_FREE;

    /* Initial chrome */
    strncpy(vb->url_current, "Loading...", sizeof(vb->url_current)-1);
    chrome_render(vb);

    /* ── Zero-copy: EGL_QUALCOMM_shared_image (WP-creates, UI-imports) ───────
     * WPEWebProcess creates the shared GPU image on its own EGL display and
     * sends {share_id, dpy} to UI over the socket once its context is current
     * (from ensure_pbuffer during first frame_will_render).  UI receives the
     * packet asynchronously in on_sdl_poll and imports via eglCreateImageKHR.
     *
     * Reversed from the previous UI-creates approach: UI's sdl_egl_dpy (0x2)
     * is not a valid display in WP's address space, causing EGL_BAD_DISPLAY
     * on import.  WP's pb_dpy (0x1) is valid in WP; UI imports using its own
     * valid sdl_egl_dpy (0x2) → no display validation error.
     *
     * Load extension pointers now (needed for import in on_sdl_poll).
     * Send sync trigger {0,0} to WP so it unblocks from its recv().
     */
    vb->pfn_CreateImage = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    vb->pfn_DestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    vb->pfn_ImageTargetTex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    fprintf(stderr, "[wpe-backend-sdl] EGLImage import fns:"
            " CreateImage=%s DestroyImage=%s ImageTargetTex=%s\n",
            vb->pfn_CreateImage    ? "YES" : "NO",
            vb->pfn_DestroyImage   ? "YES" : "NO",
            vb->pfn_ImageTargetTex ? "YES" : "NO");

    /* Send sync trigger to WPEWebProcess (content ignored by WP) */
    if (vb->host_fd >= 0) {
        EGLint sync_pkt[2] = { 0, 0 };
        ssize_t sent = write(vb->host_fd, sync_pkt, sizeof(sync_pkt));
        fprintf(stderr,
                "[wpe-backend-sdl] sync trigger sent to WPEWebProcess"
                " (%zd bytes)\n", sent);
    }

    /* ── Fallback: prism-fb mmap (CPU copy path) ──────────────────────────
     * Always set up prism-fb so it's ready if QCOM fails.
     * WP also maps prism-fb unconditionally in renderer_target_initialize.
     * repaint() uses prism-fb until qcom_ready is set via on_sdl_poll.
     */
    vb->fb_size = (size_t)vb->screen_w * vb->webkit_h * 4;
    {
        int cfd = open(PRISM_FB_PATH, O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (cfd < 0) {
            perror("[wpe-backend-sdl] create prism-fb");
        } else {
            if (ftruncate(cfd, (off_t)vb->fb_size) < 0)
                perror("[wpe-backend-sdl] ftruncate prism-fb");
            close(cfd);
            fprintf(stderr, "[wpe-backend-sdl] prism-fb created: %zu bytes\n",
                    vb->fb_size);
        }
    }
    vb->fb_fd = open(PRISM_FB_PATH, O_RDONLY);
    if (vb->fb_fd >= 0) {
        vb->fb_pixels = mmap(NULL, vb->fb_size, PROT_READ, MAP_SHARED,
                             vb->fb_fd, 0);
        if (vb->fb_pixels == MAP_FAILED) {
            perror("[wpe-backend-sdl] mmap prism-fb");
            vb->fb_pixels = NULL;
        } else {
            fprintf(stderr, "[wpe-backend-sdl] prism-fb mapped read-only\n");
        }
    } else {
        perror("[wpe-backend-sdl] open prism-fb rdonly");
    }

    /* ── Frame-signal FIFO — all WebProcesses write 'F' here ────────────────
     * Named FIFO replaces per-WP socket signals so every WPEWebProcess can
     * wake the UI regardless of IPC fd routing. */
    mkfifo("/tmp/prism-fb-frame.fifo", 0666);
    vb->frame_fifo_fd = open("/tmp/prism-fb-frame.fifo",
                             O_RDONLY | O_NONBLOCK);
    if (vb->frame_fifo_fd < 0)
        perror("[wpe-backend-sdl] frame FIFO open failed");
    else
        fprintf(stderr, "[wpe-backend-sdl] frame FIFO open: fd=%d\n",
                vb->frame_fifo_fd);

    vb->blit_ready = (vb->blit_prog && vb->cairo_prog);

    wpe_view_backend_dispatch_set_size(vb->wpe_backend, vb->screen_w, vb->webkit_h);
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
            vb->screen_w, vb->webkit_h, vb->kb_ncells);
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
        g_vb->is_bookmarked = bookmark_check(g_vb, g_vb->url_current);
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
    if (!loading) g_vb->load_progress = 0.0f;
    g_vb->chrome_dirty   = 1;
}

__attribute__((visibility("default")))
void prism_chrome_set_load_progress(double progress)
{
    if (!g_vb) return;
    g_vb->load_progress = (float)progress;
    g_vb->chrome_dirty  = 1;
}

__attribute__((visibility("default")))
void prism_chrome_push_history(const char *url)
{
    if (!g_vb || !url) return;
    history_push(g_vb, url);
}

__attribute__((visibility("default")))
void prism_set_blur_input_callback(void (*blur)(void *user), void *user)
{
    if (!g_vb) return;
    g_vb->blur_input_cb = blur;
    (void)user;  /* nav_user already set */
}

__attribute__((visibility("default")))
void prism_chrome_set_pdl_kb_visible(int visible)
{
    if (!g_vb) return;
    g_vb->pdl_kb_visible = visible;
}

__attribute__((visibility("default")))
void prism_set_home_callback(void (*home)(void *user), void *user)
{
    if (!g_vb) return;
    g_vb->home_cb = home;
    (void)user;   /* nav_user already set via prism_set_chrome_callbacks */
}

__attribute__((visibility("default")))
void prism_set_zoom_callbacks(
    void (*zoom_in)(void *user),
    void (*zoom_out)(void *user),
    void *user)
{
    if (!g_vb) return;
    g_vb->zoom_in_cb  = zoom_in;
    g_vb->zoom_out_cb = zoom_out;
    /* nav_user already set by prism_set_chrome_callbacks; user arg ignored
     * if caller passes the same pointer (which main.c does). */
    (void)user;
}

__attribute__((visibility("default")))
void prism_chrome_set_favicon(void *fav_surface)
{
    if (!g_vb) return;
    cairo_surface_t *fav = (cairo_surface_t *)fav_surface;
    /* Free old favicon */
    if (g_vb->favicon_surf) {
        cairo_surface_destroy((cairo_surface_t *)g_vb->favicon_surf);
        g_vb->favicon_surf = NULL;
    }
    if (!fav || cairo_surface_status(fav) != CAIRO_STATUS_SUCCESS) {
        g_vb->chrome_dirty = 1;
        return;
    }
    /* Scale to 20×20 ARGB32 */
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 20, 20);
    cairo_t *cr = cairo_create(dst);
    int sw = cairo_image_surface_get_width(fav);
    int sh = cairo_image_surface_get_height(fav);
    if (sw > 0 && sh > 0)
        cairo_scale(cr, 20.0 / sw, 20.0 / sh);
    cairo_set_source_surface(cr, fav, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_vb->favicon_surf = dst;
    g_vb->chrome_dirty = 1;
}

/* ── Download panel public API ──────────────────────────────────────────── */

__attribute__((visibility("default")))
int prism_download_alloc(void)
{
    if (!g_vb) return -1;
    for (int i = 0; i < DL_MAX_ENTRIES; i++) {
        if (g_vb->dl_entries[i].status == DL_STATUS_FREE) {
            g_vb->dl_entries[i].status    = DL_STATUS_ACTIVE;
            g_vb->dl_entries[i].progress  = 0.0f;
            g_vb->dl_entries[i].filename[0] = '\0';
            g_vb->dl_visible   = 1;
            g_vb->dl_dirty     = 1;
            g_vb->dl_done_at_ms = 0;
            return i;
        }
    }
    return -1; /* all slots full */
}

__attribute__((visibility("default")))
void prism_download_update(int id, const char *filename, float progress, int status)
{
    if (!g_vb || id < 0 || id >= DL_MAX_ENTRIES) return;
    if (filename && filename[0])
        strncpy(g_vb->dl_entries[id].filename, filename,
                sizeof(g_vb->dl_entries[id].filename) - 1);
    g_vb->dl_entries[id].progress = progress;
    g_vb->dl_entries[id].status   = status;
    if (status != DL_STATUS_FREE) g_vb->dl_visible = 1;
    g_vb->dl_dirty = 1;
}

__attribute__((visibility("default")))
void prism_download_free(int id)
{
    if (!g_vb || id < 0 || id >= DL_MAX_ENTRIES) return;
    g_vb->dl_entries[id].status = DL_STATUS_FREE;
    g_vb->dl_dirty = 1;
}
