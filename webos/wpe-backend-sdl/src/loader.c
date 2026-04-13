/*
 * wpe-backend-sdl: loader entry point
 *
 * libwpe calls _wpe_loader_interface.load_object(name) to get interface
 * vtable pointers by name string.  We return the appropriate struct for
 * each name that WebKit / libwpe asks for.
 */

#define WPE_COMPILATION
#include <wpe/loader.h>
#undef WPE_COMPILATION

#include "backend-egl.h"
#include "renderer-host.h"
#include "view-backend.h"

#include <string.h>
#include <stdio.h>

static void *
load_object(const char *object_name)
{
    fprintf(stderr, "[wpe-backend-sdl] load_object(%s)\n", object_name);

    if (!strcmp(object_name, "_wpe_renderer_backend_egl_interface") ||
        !strcmp(object_name,  "wpe_renderer_backend_egl_interface"))
        return (void *)&sdl_renderer_backend_egl_interface;

    if (!strcmp(object_name, "_wpe_renderer_backend_egl_target_interface") ||
        !strcmp(object_name,  "wpe_renderer_backend_egl_target_interface"))
        return (void *)&sdl_renderer_backend_egl_target_interface;

    if (!strcmp(object_name, "_wpe_renderer_backend_egl_offscreen_target_interface") ||
        !strcmp(object_name,  "wpe_renderer_backend_egl_offscreen_target_interface"))
        return (void *)&sdl_renderer_backend_egl_offscreen_target_interface;

    if (!strcmp(object_name, "_wpe_view_backend_interface") ||
        !strcmp(object_name,  "wpe_view_backend_interface"))
        return (void *)&sdl_view_backend_interface;

    if (!strcmp(object_name, "_wpe_renderer_host_interface") ||
        !strcmp(object_name,  "wpe_renderer_host_interface"))
        return (void *)&sdl_renderer_host_interface;

    fprintf(stderr, "[wpe-backend-sdl] load_object: unknown '%s'\n", object_name);
    return NULL;
}

__attribute__((constructor))
static void backend_init(void)
{
    fprintf(stderr, "[wpe-backend-sdl] library loaded\n");
}

__attribute__((visibility("default")))
const struct wpe_loader_interface _wpe_loader_interface = {
    .load_object    = load_object,
    ._wpe_reserved0 = NULL,
    ._wpe_reserved1 = NULL,
    ._wpe_reserved2 = NULL,
    ._wpe_reserved3 = NULL,
};
