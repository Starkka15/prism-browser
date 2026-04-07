/*
 * Prism Browser — minimal WPE WebKit browser shell for HP TouchPad (webOS 3.0.5)
 *
 * Usage: prism-browser [URL]
 * Default URL: https://start.duckduckgo.com
 *
 * Architecture:
 *   - Loads libWPEBackend-sdl.so as the WPE backend (SDL 1.2 + PDK EGL)
 *   - Creates a WPEWebKit web view
 *   - Runs the GLib main loop; the backend's view-backend pumps SDL events
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

/* GLib signal handler: print title changes */
static void on_title_changed(WebKitWebView *view, GParamSpec *pspec, gpointer data)
{
    const char *title = webkit_web_view_get_title(view);
    if (title && *title)
        fprintf(stderr, "[prism] title: %s\n", title);
}

/* GLib signal handler: print load progress */
static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        fprintf(stderr, "[prism] load started: %s\n",
                webkit_web_view_get_uri(view));
        break;
    case WEBKIT_LOAD_COMMITTED:
        fprintf(stderr, "[prism] load committed\n");
        break;
    case WEBKIT_LOAD_FINISHED:
        fprintf(stderr, "[prism] load finished\n");
        break;
    default:
        break;
    }
}

/* GLib signal handler: print load failures */
static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
                                const char *uri, GError *error, gpointer data)
{
    fprintf(stderr, "[prism] load failed: %s — %s\n", uri, error->message);
    return FALSE; /* let WebKit show the error page */
}

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : "https://start.duckduckgo.com";

    fprintf(stderr, "[prism] starting, url=%s\n", url);

    /* Tell libwpe which backend .so to load.
     * WPE_BACKEND env var is checked first; this is the fallback. */
    const char *backend_lib = getenv("WPE_BACKEND");
    if (!backend_lib)
        backend_lib = "libWPEBackend-sdl.so";
    wpe_loader_init(backend_lib);

    /* Create the WPE backend handle.
     * wpe_view_backend_create() calls back into the backend .so via
     * _wpe_loader_interface.load_object("wpe_view_backend_interface"). */
    struct wpe_view_backend *wpe_backend = wpe_view_backend_create();
    if (!wpe_backend) {
        fprintf(stderr, "[prism] failed to create WPE view backend\n");
        return 1;
    }

    /* Wrap in a WebKit view-backend object */
    WebKitWebViewBackend *webkit_backend =
        webkit_web_view_backend_new(wpe_backend, NULL, NULL);
    if (!webkit_backend) {
        fprintf(stderr, "[prism] failed to create WebKitWebViewBackend\n");
        return 1;
    }

    /* Configure WebKit settings */
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_page_cache(settings, FALSE);
    webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
    /* Disable features that stress a PowerVR SGX540 */
    webkit_settings_set_enable_webgl(settings, FALSE);
    /* webkit_settings_set_enable_accelerated_2d_canvas deprecated in WPE 2.38 */
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (Linux; webOS 3.0; HP TouchPad) "
        "AppleWebKit/537.36 (KHTML, like Gecko) PrismBrowser/1.0");

    /* Create the web view */
    WebKitWebView *view = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "backend", webkit_backend,
                     "settings", settings,
                     NULL));
    g_object_unref(settings);

    /* Connect signals */
    g_signal_connect(view, "notify::title",  G_CALLBACK(on_title_changed), NULL);
    g_signal_connect(view, "load-changed",   G_CALLBACK(on_load_changed),  NULL);
    g_signal_connect(view, "load-failed",    G_CALLBACK(on_load_failed),   NULL);

    /* Load the URL */
    webkit_web_view_load_uri(view, url);

    /* Run the GLib main loop — the SDL event pump is driven by the backend */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    /* Cleanup (unreachable in normal operation) */
    g_main_loop_unref(loop);
    g_object_unref(view);
    return 0;
}
