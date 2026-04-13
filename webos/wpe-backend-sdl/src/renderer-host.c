/*
 * wpe-backend-sdl: Renderer host interface
 *
 * Lives in the UI process.  Called by libwpe when spawning a new renderer
 * (WebContent) process.  create_client() returns an fd that is passed to
 * the renderer process as its host-connection end.
 *
 * For our simple fullscreen-SDL backend, we create a socketpair:
 *   - one end goes to the renderer (returned from create_client)
 *   - the other end is stored in the host object but not actively used
 *     (frame sync is handled separately via the view-backend socketpair)
 */

#include "renderer-host.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <stdio.h>

struct sdl_renderer_host {
    int host_fd; /* UI-process end of the host socketpair (not used for I/O) */
};

static void *
renderer_host_create(void)
{
    struct sdl_renderer_host *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->host_fd = -1;
    fprintf(stderr, "[wpe-backend-sdl] renderer_host_create\n");
    return h;
}

static void
renderer_host_destroy(void *data)
{
    struct sdl_renderer_host *h = data;
    if (!h) return;
    if (h->host_fd >= 0)
        close(h->host_fd);
    free(h);
}

static int
renderer_host_create_client(void *data)
{
    struct sdl_renderer_host *h = data;

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        fprintf(stderr, "[wpe-backend-sdl] renderer_host_create_client: socketpair failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* If we already have a host fd from a previous client, close it */
    if (h->host_fd >= 0)
        close(h->host_fd);

    h->host_fd = fds[0]; /* UI process keeps this end */
    fprintf(stderr, "[wpe-backend-sdl] renderer_host_create_client: client_fd=%d host_fd=%d\n",
            fds[1], fds[0]);

    return fds[1]; /* renderer process gets this end */
}

const struct wpe_renderer_host_interface sdl_renderer_host_interface = {
    .create        = renderer_host_create,
    .destroy       = renderer_host_destroy,
    .create_client = renderer_host_create_client,
    ._wpe_reserved0 = NULL,
    ._wpe_reserved1 = NULL,
    ._wpe_reserved2 = NULL,
    ._wpe_reserved3 = NULL,
};
