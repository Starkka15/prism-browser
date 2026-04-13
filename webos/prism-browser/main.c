/*
 * Prism Browser — WPE WebKit browser shell for HP TouchPad (webOS 3.0.5)
 *
 * Loads libWPEBackend-sdl.so (RTLD_GLOBAL), then resolves the chrome API
 * functions via dlsym to wire navigation callbacks and keep the toolbar in
 * sync with WebKit's load state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include <PDL.h>
#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

/* ── webOS PDL input method context ─────────────────────────────────────────
 * Subclass of WebKitInputMethodContext that drives the native webOS on-screen
 * keyboard.  WebKit calls notify_focus_in when a text field gains focus and
 * notify_focus_out when it loses focus.  All other methods are no-ops.
 * ─────────────────────────────────────────────────────────────────────────── */

#define PRISM_TYPE_IME (prism_ime_get_type())
G_DECLARE_FINAL_TYPE(PrismIme, prism_ime, PRISM, IME, WebKitInputMethodContext)

struct _PrismIme { WebKitInputMethodContext parent; };

G_DEFINE_TYPE(PrismIme, prism_ime, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

static void prism_ime_notify_focus_in(WebKitInputMethodContext *ctx)
{
    (void)ctx;
    fprintf(stderr, "[prism] IME focus_in → PDL keyboard open\n");
    PDL_SetKeyboardState(PDL_TRUE);
}

static void prism_ime_notify_focus_out(WebKitInputMethodContext *ctx)
{
    (void)ctx;
    fprintf(stderr, "[prism] IME focus_out → PDL keyboard close\n");
    PDL_SetKeyboardState(PDL_FALSE);
}

static gboolean prism_ime_filter_key_event(WebKitInputMethodContext *ctx,
                                            struct wpe_input_keyboard_event *event)
{
    (void)ctx; (void)event;
    return FALSE; /* don't consume — WebKit handles the key */
}

static void prism_ime_get_preedit(WebKitInputMethodContext *ctx,
                                   char **text, GList **underlines, guint *cursor)
{
    (void)ctx;
    if (text)       *text       = NULL;
    if (underlines) *underlines = NULL;
    if (cursor)     *cursor     = 0;
}

static void prism_ime_class_init(PrismImeClass *klass)
{
    WebKitInputMethodContextClass *imc = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(klass);
    imc->notify_focus_in  = prism_ime_notify_focus_in;
    imc->notify_focus_out = prism_ime_notify_focus_out;
    imc->filter_key_event = prism_ime_filter_key_event;
    imc->get_preedit      = prism_ime_get_preedit;
}

static void prism_ime_init(PrismIme *self) { (void)self; }

/* ── Chrome API function pointers (resolved via dlsym after dlopen) ──────── */

typedef void (*fn_set_callbacks)(
    void (*navigate)(const char *, void *),
    void (*go_back)(void *),
    void (*go_fwd)(void *),
    void (*reload)(void *),
    void (*stop)(void *),
    void *user);

typedef void (*fn_set_url)(const char *url);
typedef void (*fn_set_nav_state)(int can_back, int can_fwd, int loading);

static fn_set_callbacks  chrome_set_callbacks  = NULL;
static fn_set_url        chrome_set_url        = NULL;
static fn_set_nav_state  chrome_set_nav_state  = NULL;

/* ── Navigation callbacks ───────────────────────────────────────────────── */

static void cb_navigate(const char *url, void *user)
{
    fprintf(stderr, "[prism] navigate → %s\n", url);
    webkit_web_view_load_uri((WebKitWebView *)user, url);
}

static void cb_go_back(void *user)
{
    webkit_web_view_go_back((WebKitWebView *)user);
}

static void cb_go_fwd(void *user)
{
    webkit_web_view_go_forward((WebKitWebView *)user);
}

static void cb_reload(void *user)
{
    webkit_web_view_reload((WebKitWebView *)user);
}

static void cb_stop(void *user)
{
    webkit_web_view_stop_loading((WebKitWebView *)user);
}

/* ── Sync chrome state from WebKit ──────────────────────────────────────── */

static void sync_chrome(WebKitWebView *view, int loading)
{
    if (chrome_set_url) {
        const char *uri = webkit_web_view_get_uri(view);
        chrome_set_url(uri ? uri : "");
    }
    if (chrome_set_nav_state) {
        chrome_set_nav_state(
            webkit_web_view_can_go_back(view)    ? 1 : 0,
            webkit_web_view_can_go_forward(view) ? 1 : 0,
            loading);
    }
}

/* ── WebKit signal handlers ─────────────────────────────────────────────── */

static void on_title_changed(WebKitWebView *view, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    const char *title = webkit_web_view_get_title(view);
    if (title && *title)
        fprintf(stderr, "[prism] title: %s\n", title);
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event,
                             gpointer data)
{
    (void)data;
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        fprintf(stderr, "[prism] load started: %s\n", webkit_web_view_get_uri(view));
        sync_chrome(view, 1);
        break;
    case WEBKIT_LOAD_COMMITTED:
        sync_chrome(view, 1);
        break;
    case WEBKIT_LOAD_FINISHED:
        fprintf(stderr, "[prism] load finished\n");
        sync_chrome(view, 0);
        break;
    default:
        break;
    }
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
                                const char *uri, GError *error, gpointer data)
{
    (void)data; (void)event;
    fprintf(stderr, "[prism] load failed: %s — %s\n", uri, error->message);
    sync_chrome(view, 0);
    return FALSE;
}

static void on_uri_changed(WebKitWebView *view, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    /* Keep URL bar in sync during redirects */
    if (chrome_set_url) {
        const char *uri = webkit_web_view_get_uri(view);
        chrome_set_url(uri ? uri : "");
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : "http://info.cern.ch";
    fprintf(stderr, "[prism] starting, url=%s\n", url);

    PDL_Err pdl_err = PDL_Init(0);
    if (pdl_err != PDL_NOERROR)
        fprintf(stderr, "[prism] PDL_Init failed: %d\n", pdl_err);
    else
        fprintf(stderr, "[prism] PDL_Init OK\n");

    const char *backend_lib = getenv("WPE_BACKEND");
    if (!backend_lib)
        backend_lib = "libWPEBackend-sdl.so";

    /* Load backend with RTLD_GLOBAL so exported symbols are findable via dlsym */
    dlerror();
    void *backend_handle = dlopen(backend_lib, RTLD_NOW | RTLD_GLOBAL);
    if (!backend_handle) {
        fprintf(stderr, "[prism] dlopen failed: %s\n", dlerror());
        return 1;
    }
    fprintf(stderr, "[prism] backend dlopen OK\n");

    /* Resolve chrome API */
    chrome_set_callbacks = (fn_set_callbacks) dlsym(backend_handle, "prism_set_chrome_callbacks");
    chrome_set_url       = (fn_set_url)       dlsym(backend_handle, "prism_chrome_set_url");
    chrome_set_nav_state = (fn_set_nav_state) dlsym(backend_handle, "prism_chrome_set_nav_state");
    fprintf(stderr, "[prism] chrome API: callbacks=%p url=%p navstate=%p\n",
            (void *)chrome_set_callbacks,
            (void *)chrome_set_url,
            (void *)chrome_set_nav_state);

    wpe_loader_init(backend_lib);

    struct wpe_view_backend *wpe_backend = wpe_view_backend_create();
    if (!wpe_backend) {
        fprintf(stderr, "[prism] failed to create WPE view backend\n");
        return 1;
    }

    WebKitWebViewBackend *webkit_backend =
        webkit_web_view_backend_new(wpe_backend, NULL, NULL);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_page_cache(settings, FALSE);
    webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (Linux; webOS 3.0; HP TouchPad) "
        "AppleWebKit/537.36 (KHTML, like Gecko) PrismBrowser/1.0");

    WebKitWebView *view = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "backend",  webkit_backend,
                     "settings", settings,
                     NULL));
    g_object_unref(settings);

    /* Install our PDL input method context so WebKit's focus_in/out drive the
     * native webOS keyboard instead of whatever the default would do. */
    PrismIme *ime = g_object_new(PRISM_TYPE_IME, NULL);
    webkit_web_view_set_input_method_context(view, WEBKIT_INPUT_METHOD_CONTEXT(ime));
    g_object_unref(ime);

    /* Wire chrome navigation callbacks (view is created, backend is initialised) */
    if (chrome_set_callbacks) {
        chrome_set_callbacks(cb_navigate, cb_go_back, cb_go_fwd,
                             cb_reload, cb_stop, view);
        fprintf(stderr, "[prism] chrome callbacks wired\n");
    }
    if (chrome_set_url)
        chrome_set_url(url);

    /* WebKit signals */
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title_changed), NULL);
    g_signal_connect(view, "notify::uri",   G_CALLBACK(on_uri_changed),   NULL);
    g_signal_connect(view, "load-changed",  G_CALLBACK(on_load_changed),  NULL);
    g_signal_connect(view, "load-failed",   G_CALLBACK(on_load_failed),   NULL);

    webkit_web_view_load_uri(view, url);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(view);
    PDL_Quit();
    return 0;
}
