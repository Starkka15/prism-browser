/*
 * wpe-backend-sdl: View backend for HP TouchPad (SDL 1.2 + PDK input)
 *
 * Lives in the UI process.  Responsible for:
 *   - Initialising SDL 1.2 (video + events)
 *   - Pumping SDL events and translating them to WPE input events
 *   - Receiving frame-done signals from the renderer over a Unix socket
 *     and calling wpe_view_backend_dispatch_frame_displayed()
 *
 * TouchPad specifics:
 *   - Screen: 1024 × 768 landscape
 *   - Input: capacitive multi-touch; delivered as SDL mouse events (single
 *     primary touch) from PDK SDL 1.2.  We also handle the SDLKey back
 *     gesture (SDLK_ESCAPE / SDLK_F1 on PDK).
 *   - SDL_QUIT is raised by the webOS activity manager (app suspend/kill).
 */

#include "view-backend.h"
#include "keysym-map.h"

#include <wpe/wpe.h>
/* wpe/input.h is pulled in by wpe/wpe.h — do not include directly */

#include <SDL/SDL.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>

/* ── Helpers ────────────────────────────────────────────────────────────── */

static uint32_t
now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* ── SDL key → WPE key translation ─────────────────────────────────────── */

static void
dispatch_key(struct sdl_view_backend *vb, SDL_KeyboardEvent *ke)
{
    struct wpe_input_keyboard_event ev = {
        .time             = now_ms(),
        .key_code         = sdlkey_to_wpe(ke->keysym.sym),
        .hardware_key_code = (uint32_t)ke->keysym.scancode,
        .pressed          = (ke->type == SDL_KEYDOWN),
        .modifiers        = sdl_mod_to_wpe(ke->keysym.mod),
    };
    wpe_view_backend_dispatch_keyboard_event(vb->wpe_backend, &ev);
}

/* ── SDL mouse → WPE touch ──────────────────────────────────────────────── */

static void
dispatch_touch(struct sdl_view_backend *vb, int x, int y,
               enum wpe_input_touch_event_type type)
{
    struct wpe_input_touch_event_raw raw = {
        .type = type,
        .time = now_ms(),
        .id   = 0,
        .x    = x,
        .y    = y,
    };
    struct wpe_input_touch_event ev = {
        .touchpoints        = &raw,
        .touchpoints_length = 1,
        .type               = type,
        .id                 = 0,
        .time               = raw.time,
        .modifiers          = 0,
    };
    wpe_view_backend_dispatch_touch_event(vb->wpe_backend, &ev);
}

/* ── Scroll (mouse wheel) ───────────────────────────────────────────────── */

static void
dispatch_scroll(struct sdl_view_backend *vb, SDL_MouseButtonEvent *be)
{
    int delta = (be->button == SDL_BUTTON_WHEELUP) ? -120 : 120;
    struct wpe_input_axis_event ev = {
        .type      = wpe_input_axis_event_type_motion,
        .time      = now_ms(),
        .x         = be->x,
        .y         = be->y,
        .axis      = 1, /* vertical scroll */
        .value     = delta,
        .modifiers = 0,
    };
    wpe_view_backend_dispatch_axis_event(vb->wpe_backend, &ev);
}

/* ── Main SDL event pump ────────────────────────────────────────────────── */

static void
pump_sdl_events(struct sdl_view_backend *vb)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            dispatch_key(vb, &e.key);
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_WHEELUP ||
                e.button.button == SDL_BUTTON_WHEELDOWN) {
                dispatch_scroll(vb, &e.button);
                break;
            }
            if (e.button.button == SDL_BUTTON_LEFT) {
                vb->touch_active = 1;
                vb->touch_x = e.button.x;
                vb->touch_y = e.button.y;
                dispatch_touch(vb, e.button.x, e.button.y,
                               wpe_input_touch_event_type_down);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT && vb->touch_active) {
                vb->touch_active = 0;
                dispatch_touch(vb, e.button.x, e.button.y,
                               wpe_input_touch_event_type_up);
            }
            break;

        case SDL_MOUSEMOTION:
            if (vb->touch_active) {
                vb->touch_x = e.motion.x;
                vb->touch_y = e.motion.y;
                dispatch_touch(vb, e.motion.x, e.motion.y,
                               wpe_input_touch_event_type_motion);
            }
            break;

        case SDL_QUIT:
            /* webOS activity manager killed us — clean exit */
            exit(0);
            break;

        default:
            break;
        }
    }
}

/* ── View backend interface ─────────────────────────────────────────────── */

static void *
view_backend_create(void *params, struct wpe_view_backend *wpe_backend)
{
    (void)params;

    struct sdl_view_backend *vb = calloc(1, sizeof(*vb));
    if (!vb)
        return NULL;

    vb->wpe_backend = wpe_backend;
    vb->host_fd      = -1;
    vb->renderer_fd  = -1;
    vb->touch_active = 0;

    /* Create socket pair for frame-done IPC */
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        free(vb);
        return NULL;
    }
    vb->host_fd     = fds[0]; /* we poll this for 'F' bytes from renderer */
    vb->renderer_fd = fds[1]; /* given to the renderer backend via WPE     */

    return vb;
}

static void
view_backend_destroy(void *data)
{
    struct sdl_view_backend *vb = data;
    if (!vb) return;
    if (vb->host_fd >= 0)     close(vb->host_fd);
    if (vb->renderer_fd >= 0) close(vb->renderer_fd);
    free(vb);
}

static void
view_backend_initialize(void *data)
{
    struct sdl_view_backend *vb = data;

    /* SDL may already be initialized by the browser shell, but be safe */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE) < 0) {
            fprintf(stderr, "wpe-backend-sdl: SDL_Init failed: %s\n",
                    SDL_GetError());
            return;
        }
    }

    /* Set fullscreen video mode — this also creates the PDK EGL surface */
    if (!SDL_SetVideoMode(WEBOS_SCREEN_W, WEBOS_SCREEN_H, 0,
                          SDL_SWSURFACE | SDL_NOFRAME)) {
        fprintf(stderr, "wpe-backend-sdl: SDL_SetVideoMode failed: %s\n",
                SDL_GetError());
        return;
    }

    SDL_ShowCursor(SDL_DISABLE);

    /* Tell WPE our size */
    wpe_view_backend_dispatch_set_size(vb->wpe_backend,
                                       WEBOS_SCREEN_W, WEBOS_SCREEN_H);

    /* Mark the view as visible + focused */
    wpe_view_backend_add_activity_state(vb->wpe_backend,
        wpe_view_activity_state_visible  |
        wpe_view_activity_state_focused  |
        wpe_view_activity_state_in_window);
}

static int
view_backend_get_renderer_host_fd(void *data)
{
    struct sdl_view_backend *vb = data;
    /*
     * Return renderer_fd — WPE will pass this to the WebContent process,
     * which calls wpe_renderer_backend_egl_create(renderer_fd).
     * After returning, we no longer own this fd (WPE takes it).
     */
    int fd = vb->renderer_fd;
    vb->renderer_fd = -1; /* don't close it in destroy */
    return fd;
}

const struct wpe_view_backend_interface sdl_view_backend_interface = {
    .create                = view_backend_create,
    .destroy               = view_backend_destroy,
    .initialize            = view_backend_initialize,
    .get_renderer_host_fd  = view_backend_get_renderer_host_fd,
    ._wpe_reserved0        = NULL,
    ._wpe_reserved1        = NULL,
    ._wpe_reserved2        = NULL,
    ._wpe_reserved3        = NULL,
};

/*
 * Called by the browser shell's main loop to pump events AND to drain any
 * frame-done signals from the renderer, driving WPE's frame scheduling.
 *
 * The shell should call this once per iteration of its GLib main loop
 * (or directly from a GSource prepare/dispatch).
 */
void
wpe_sdl_dispatch(struct sdl_view_backend *vb)
{
    pump_sdl_events(vb);

    /* Non-blocking drain of frame-done signals */
    if (vb->host_fd >= 0) {
        char buf[32];
        ssize_t n;
        while ((n = recv(vb->host_fd, buf, sizeof(buf),
                         MSG_DONTWAIT)) > 0) {
            /* One 'F' byte = one frame displayed */
            for (ssize_t i = 0; i < n; i++)
                wpe_view_backend_dispatch_frame_displayed(vb->wpe_backend);
        }
    }
}
