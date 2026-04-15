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
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

#include <PDL.h>
#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

#define PRISM_DATA_DIR      "/media/internal/prism/data"
#define PRISM_CACHE_DIR     "/tmp/prism-cache"    /* tmpfs: supports hard links */
#define PRISM_COOKIE_DB     "/media/internal/prism/cookies.sqlite"
#define PRISM_DOWNLOAD_DIR  "/media/internal/downloads"

/* Forward-declared so IME callbacks can call it before the full chrome API block */
static void (*chrome_pdl_kb_visible)(int visible) = NULL;

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
    if (chrome_pdl_kb_visible) chrome_pdl_kb_visible(1);
    PDL_SetKeyboardState(PDL_TRUE);
}

static void prism_ime_notify_focus_out(WebKitInputMethodContext *ctx)
{
    (void)ctx;
    fprintf(stderr, "[prism] IME focus_out → PDL keyboard close\n");
    if (chrome_pdl_kb_visible) chrome_pdl_kb_visible(0);
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

typedef void (*fn_set_zoom_callbacks)(
    void (*zoom_in)(void *user),
    void (*zoom_out)(void *user),
    void *user);

typedef void (*fn_set_home_callback)(void (*home)(void *user), void *user);
typedef void (*fn_set_blur_input_callback)(void (*blur)(void *user), void *user);
typedef void (*fn_pdl_kb_visible_fn)(int visible);

typedef int  (*fn_dl_alloc)(void);
typedef void (*fn_dl_update)(int id, const char *filename, float progress, int status);
typedef void (*fn_dl_free)(int id);

typedef void (*fn_set_url)(const char *url);
typedef void (*fn_set_nav_state)(int can_back, int can_fwd, int loading);
typedef void (*fn_set_load_progress)(double progress);
typedef void (*fn_push_history)(const char *url);

static fn_set_callbacks           chrome_set_callbacks           = NULL;
static fn_set_zoom_callbacks      chrome_set_zoom_callbacks      = NULL;
static fn_set_home_callback       chrome_set_home_callback       = NULL;
static fn_set_blur_input_callback chrome_set_blur_input_callback = NULL;
static fn_dl_alloc                chrome_dl_alloc                = NULL;
static fn_dl_update               chrome_dl_update               = NULL;
static fn_dl_free                 chrome_dl_free                 = NULL;

static char g_home_url[2048] = "";
static WebKitWebView *g_view = NULL;   /* set after view creation */

/* ── Session restore ────────────────────────────────────────────────────── */
#define PRISM_SESSION_FILE "/media/internal/prism/session.txt"

static void session_save(const char *url)
{
    if (!url || !url[0]) return;
    if (strncmp(url, "about:", 6) == 0) return;
    if (strncmp(url, "file://",  7) == 0) return;
    FILE *f = fopen(PRISM_SESSION_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", url);
    fclose(f);
}

static const char *session_load(void)
{
    static char buf[2048];
    FILE *f = fopen(PRISM_SESSION_FILE, "r");
    if (!f) return NULL;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
    return buf[0] ? buf : NULL;
}
static fn_set_url            chrome_set_url            = NULL;
static fn_set_nav_state      chrome_set_nav_state      = NULL;
static fn_set_load_progress  chrome_set_load_progress  = NULL;
static fn_push_history       chrome_push_history       = NULL;

/* ── File URL loader (bypasses file:// security via load_html) ──────────── */

static void load_file_url(WebKitWebView *view, const char *url)
{
    /* Strip "file://" prefix to get the filesystem path */
    const char *raw = url + sizeof("file://") - 1;

    /* Remove query/fragment for filesystem access */
    char path[2048];
    strncpy(path, raw, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *q = strchr(path, '?');  if (q) *q = '\0';
    char *h = strchr(path, '#');  if (h) *h = '\0';

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[prism] load_file_url: cannot open %s: %s\n",
                path, strerror(errno));
        webkit_web_view_load_uri(view, "about:blank");
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *html = malloc((size_t)len + 1);
    if (!html) { fclose(f); return; }
    fread(html, 1, (size_t)len, f);
    html[len] = '\0';
    fclose(f);

    char path2[2048];
    strncpy(path2, path, sizeof(path2) - 1);
    char base_uri[512];
    snprintf(base_uri, sizeof(base_uri), "file://%s/", dirname(path2));
    webkit_web_view_load_html(view, html, base_uri);
    free(html);
}

/* ── Download support ───────────────────────────────────────────────────── */

static void on_download_progress(WebKitDownload *dl, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dl), "prism-dl-slot"));
    if (slot < 0 || !chrome_dl_update) return;
    double pct = webkit_download_get_estimated_progress(dl);
    const char *dest = webkit_download_get_destination(dl);
    const char *name = dest ? strrchr(dest, '/') : NULL;
    name = (name && name[1]) ? name + 1 : NULL;
    chrome_dl_update(slot, name, (float)pct, 0 /* ACTIVE */);
}

static void on_download_finished(WebKitDownload *dl, gpointer data)
{
    (void)data;
    const char *dest = webkit_download_get_destination(dl);
    guint64 bytes = webkit_download_get_received_data_length(dl);
    fprintf(stderr, "[prism] download finished: %s (%llu bytes)\n",
            dest ? dest : "(null)", (unsigned long long)bytes);
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dl), "prism-dl-slot"));
    if (slot >= 0 && chrome_dl_update)
        chrome_dl_update(slot, NULL, 1.0f, 1 /* DONE */);
}

static void on_download_failed(WebKitDownload *dl, GError *error, gpointer data)
{
    (void)data;
    const char *dest = webkit_download_get_destination(dl);
    fprintf(stderr, "[prism] download failed: %s — %s\n",
            dest ? dest : "(null)", error->message);
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dl), "prism-dl-slot"));
    if (slot >= 0 && chrome_dl_update)
        chrome_dl_update(slot, NULL, 0.0f, 2 /* FAILED */);
}

/* Build a unique destination path, adding " (N)" suffix if file already exists */
static void make_dest_uri(const char *dir, const char *filename,
                           char *out, size_t out_sz)
{
    /* Split filename into base and extension */
    char base[256], ext[64];
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) {
        size_t blen = (size_t)(dot - filename);
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, filename, blen);
        base[blen] = '\0';
        strncpy(ext, dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
    } else {
        strncpy(base, filename, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        ext[0] = '\0';
    }

    /* Try plain name first, then add counter */
    snprintf(out, out_sz, "file://%s/%s%s", dir, base, ext);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s%s", dir, base, ext);
    if (access(path, F_OK) != 0) return;   /* doesn't exist yet — use it */

    for (int n = 1; n < 100; n++) {
        snprintf(out, out_sz, "file://%s/%s (%d)%s", dir, base, n, ext);
        snprintf(path, sizeof(path), "%s/%s (%d)%s", dir, base, n, ext);
        if (access(path, F_OK) != 0) return;
    }
}

static gboolean on_decide_destination(WebKitDownload *dl,
                                       const gchar *suggested,
                                       gpointer data)
{
    (void)data;

    /* Use suggested filename if provided, otherwise parse from URI */
    const char *name = (suggested && suggested[0]) ? suggested : NULL;
    char name_buf[256] = "download";

    if (!name) {
        WebKitURIRequest *req = webkit_download_get_request(dl);
        const char *uri = webkit_uri_request_get_uri(req);
        const char *slash = strrchr(uri, '/');
        const char *raw = (slash && slash[1]) ? slash + 1 : uri;
        strncpy(name_buf, raw, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        /* Strip query/fragment */
        char *q = strchr(name_buf, '?'); if (q) *q = '\0';
        char *h = strchr(name_buf, '#'); if (h) *h = '\0';
        if (!name_buf[0]) strncpy(name_buf, "download", sizeof(name_buf) - 1);
        name = name_buf;
    } else {
        /* Sanitize: strip any path component */
        const char *slash2 = strrchr(name, '/');
        if (slash2 && slash2[1]) name = slash2 + 1;
        strncpy(name_buf, name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        name = name_buf;
    }

    char dest_uri[600];
    make_dest_uri(PRISM_DOWNLOAD_DIR, name, dest_uri, sizeof(dest_uri));

    webkit_download_set_destination(dl, dest_uri);
    fprintf(stderr, "[prism] download destination: %s\n", dest_uri);

    /* Update panel with resolved filename now that we know it */
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dl), "prism-dl-slot"));
    if (slot >= 0 && chrome_dl_update)
        chrome_dl_update(slot, name, 0.0f, 0 /* ACTIVE */);

    return TRUE;
}

static void on_download_started(WebKitWebContext *ctx, WebKitDownload *dl,
                                 gpointer data)
{
    (void)ctx; (void)data;
    WebKitURIRequest *req = webkit_download_get_request(dl);
    fprintf(stderr, "[prism] download started: %s\n",
            webkit_uri_request_get_uri(req));
    int slot = chrome_dl_alloc ? chrome_dl_alloc() : -1;
    g_object_set_data(G_OBJECT(dl), "prism-dl-slot", GINT_TO_POINTER(slot));
    fprintf(stderr, "[prism] download slot: %d\n", slot);
    webkit_download_set_allow_overwrite(dl, FALSE);
    g_signal_connect(dl, "decide-destination",           G_CALLBACK(on_decide_destination), NULL);
    g_signal_connect(dl, "notify::estimated-progress",   G_CALLBACK(on_download_progress),  NULL);
    g_signal_connect(dl, "finished",                     G_CALLBACK(on_download_finished),  NULL);
    g_signal_connect(dl, "failed",                       G_CALLBACK(on_download_failed),    NULL);
}

/* ── Navigation policy: intercept file:// links ─────────────────────────── */

static gboolean on_decide_policy(WebKitWebView *view,
                                  WebKitPolicyDecision *decision,
                                  WebKitPolicyDecisionType type,
                                  gpointer user_data)
{
    (void)user_data;

    /* New-window policy: no tabs — redirect target="_blank" into current view */
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision *nav =
            WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(nav);
        WebKitURIRequest *req = webkit_navigation_action_get_request(action);
        const char *uri = webkit_uri_request_get_uri(req);
        fprintf(stderr, "[prism] new-window → current view: %s\n", uri ? uri : "(null)");
        webkit_policy_decision_ignore(decision);
        if (uri && uri[0]) {
            if (strncmp(uri, "file://", 7) == 0)
                load_file_url(view, uri);
            else
                webkit_web_view_load_uri(view, uri);
        }
        return TRUE;
    }

    /* Response policy: trigger download for unsupported MIME types */
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *resp =
            WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(resp)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
        return FALSE;
    }

    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;

    WebKitNavigationPolicyDecision *nav =
        WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(nav);

    /* Only intercept user-initiated navigations (link clicks, form submits).
     * Programmatic loads from load_html()/load_uri() use WEBKIT_NAVIGATION_TYPE_OTHER
     * and must NOT be intercepted — doing so causes load_html() to cancel itself. */
    WebKitNavigationType nav_type = webkit_navigation_action_get_navigation_type(action);
    if (nav_type != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
        nav_type != WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED &&
        nav_type != WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED)
        return FALSE;

    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const char *uri = webkit_uri_request_get_uri(req);

    /* javascript: pseudo-protocol — evaluate the expression in the current page
     * context.  WPEWebKit may block or no-op javascript: URI navigations for
     * link clicks; explicitly evaluate instead.  This is what makes
     * appcatalog.webosarchive.org's getLink() download links work. */
    if (uri && strncmp(uri, "javascript:", 11) == 0) {
        fprintf(stderr, "[prism] policy: javascript: link → evaluating\n");
        webkit_policy_decision_ignore(decision);
        webkit_web_view_run_javascript(view, uri + 11, NULL, NULL, NULL);
        return TRUE;
    }

    if (!uri || strncmp(uri, "file://", 7) != 0)
        return FALSE;  /* let WebKit handle http/https/etc. normally */

    fprintf(stderr, "[prism] policy: file:// link → %s\n", uri);
    webkit_policy_decision_ignore(decision);
    load_file_url(view, uri);
    return TRUE;
}

/* ── Navigation callbacks ───────────────────────────────────────────────── */

/* Sites known to be too heavy for the TouchPad — redirect to lighter versions. */
static const char *apply_site_redirects(const char *url)
{
    /* duckduckgo.com → lite.duckduckgo.com */
    if (strstr(url, "://duckduckgo.com") || strstr(url, "://www.duckduckgo.com"))
        return "https://lite.duckduckgo.com/lite/";
    return url;
}

static void cb_navigate(const char *url, void *user)
{
    const char *redirected = apply_site_redirects(url);
    if (redirected != url)
        fprintf(stderr, "[prism] redirect: %s → %s\n", url, redirected);
    else
        fprintf(stderr, "[prism] navigate → %s\n", url);
    WebKitWebView *view = (WebKitWebView *)user;
    if (strncmp(redirected, "file://", 7) == 0)
        load_file_url(view, redirected);
    else
        webkit_web_view_load_uri(view, redirected);
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

static void cb_home(void *user)
{
    WebKitWebView *view = (WebKitWebView *)user;
    fprintf(stderr, "[prism] home → %s\n", g_home_url);
    load_file_url(view, g_home_url);
}

static void cb_zoom_in(void *user)
{
    WebKitWebView *view = (WebKitWebView *)user;
    double level = webkit_web_view_get_zoom_level(view);
    double next  = level * 1.25;
    if (next > 5.0) next = 5.0;
    webkit_web_view_set_zoom_level(view, next);
    fprintf(stderr, "[prism] zoom in: %.2f → %.2f\n", level, next);
}

static void cb_zoom_out(void *user)
{
    WebKitWebView *view = (WebKitWebView *)user;
    double level = webkit_web_view_get_zoom_level(view);
    double next  = level * 0.80;
    if (next < 0.25) next = 0.25;
    webkit_web_view_set_zoom_level(view, next);
    fprintf(stderr, "[prism] zoom out: %.2f → %.2f\n", level, next);
}

static void cb_blur_input(void *user)
{
    (void)user;
    if (!g_view) return;
    fprintf(stderr, "[prism] blur_input → JS blur\n");
    webkit_web_view_run_javascript(g_view,
        "if(document.activeElement)document.activeElement.blur();",
        NULL, NULL, NULL);
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
        {
            const char *uri = webkit_web_view_get_uri(view);
            if (uri) {
                if (chrome_push_history) chrome_push_history(uri);
                session_save(uri);
            }
        }
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

static void on_progress_changed(WebKitWebView *view, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    if (chrome_set_load_progress)
        chrome_set_load_progress(webkit_web_view_get_estimated_load_progress(view));
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1]
        : "file:///usr/palm/applications/com.prism.browser/home.html";
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
    chrome_set_callbacks           = (fn_set_callbacks)           dlsym(backend_handle, "prism_set_chrome_callbacks");
    chrome_set_zoom_callbacks      = (fn_set_zoom_callbacks)      dlsym(backend_handle, "prism_set_zoom_callbacks");
    chrome_set_home_callback       = (fn_set_home_callback)       dlsym(backend_handle, "prism_set_home_callback");
    chrome_set_blur_input_callback = (fn_set_blur_input_callback) dlsym(backend_handle, "prism_set_blur_input_callback");
    chrome_set_url                 = (fn_set_url)                 dlsym(backend_handle, "prism_chrome_set_url");
    chrome_set_nav_state           = (fn_set_nav_state)           dlsym(backend_handle, "prism_chrome_set_nav_state");
    chrome_set_load_progress       = (fn_set_load_progress)       dlsym(backend_handle, "prism_chrome_set_load_progress");
    chrome_push_history            = (fn_push_history)            dlsym(backend_handle, "prism_chrome_push_history");
    chrome_pdl_kb_visible          = (fn_pdl_kb_visible_fn)       dlsym(backend_handle, "prism_chrome_set_pdl_kb_visible");
    chrome_dl_alloc                = (fn_dl_alloc)                dlsym(backend_handle, "prism_download_alloc");
    chrome_dl_update               = (fn_dl_update)               dlsym(backend_handle, "prism_download_update");
    chrome_dl_free                 = (fn_dl_free)                 dlsym(backend_handle, "prism_download_free");
    fprintf(stderr, "[prism] chrome API: callbacks=%p zoom=%p url=%p navstate=%p"
            " progress=%p push_history=%p blur=%p pdlkb=%p dl=%p/%p/%p\n",
            (void *)chrome_set_callbacks,
            (void *)chrome_set_zoom_callbacks,
            (void *)chrome_set_url,
            (void *)chrome_set_nav_state,
            (void *)chrome_set_load_progress,
            (void *)chrome_push_history,
            (void *)chrome_set_blur_input_callback,
            (void *)chrome_pdl_kb_visible,
            (void *)chrome_dl_alloc,
            (void *)chrome_dl_update,
            (void *)chrome_dl_free);

    wpe_loader_init(backend_lib);

    struct wpe_view_backend *wpe_backend = wpe_view_backend_create();
    if (!wpe_backend) {
        fprintf(stderr, "[prism] failed to create WPE view backend\n");
        return 1;
    }

    WebKitWebViewBackend *webkit_backend =
        webkit_web_view_backend_new(wpe_backend, NULL, NULL);

    /* ── Persistent data/cache/cookie storage ───────────────────────────────
     * Store everything under /media/internal/prism/ — this partition is
     * on the encrypted user storage and persists across app restarts and
     * reboots.  Cookies, localStorage, and cached resources survive sessions.
     */
    mkdir("/media/internal/prism",       0755);
    mkdir(PRISM_DATA_DIR,                0755);
    mkdir(PRISM_CACHE_DIR,               0755);
    /* /media/internal/downloads already exists on stock webOS — just ensure it */
    mkdir(PRISM_DOWNLOAD_DIR, 0755);

    WebKitWebsiteDataManager *data_mgr = webkit_website_data_manager_new(
        "base-data-directory",  PRISM_DATA_DIR,
        "base-cache-directory", PRISM_CACHE_DIR,
        NULL);

    WebKitWebContext *context =
        webkit_web_context_new_with_website_data_manager(data_mgr);
    g_object_unref(data_mgr);

    /* Persistent cookie storage (SQLite) — survives process restarts */
    WebKitCookieManager *cookie_mgr =
        webkit_web_context_get_cookie_manager(context);
    webkit_cookie_manager_set_persistent_storage(
        cookie_mgr,
        PRISM_COOKIE_DB,
        WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_cookie_manager_set_accept_policy(
        cookie_mgr,
        WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    fprintf(stderr, "[prism] cookie storage: %s\n", PRISM_COOKIE_DB);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_page_cache(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (Linux; Android 10; Tablet) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36");

    /* Download-started fires on context — connect before unref (view holds the ref) */
    g_signal_connect(context, "download-started", G_CALLBACK(on_download_started), NULL);

    WebKitWebView *view = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "backend",     webkit_backend,
                     "settings",    settings,
                     "web-context", context,
                     NULL));
    g_object_unref(context);
    g_object_unref(settings);

    g_view = view;

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
    if (chrome_set_zoom_callbacks) {
        chrome_set_zoom_callbacks(cb_zoom_in, cb_zoom_out, view);
        fprintf(stderr, "[prism] zoom callbacks wired\n");
    }
    if (chrome_set_home_callback) {
        /* Use the startup file:// URL if present (wrapper provides correct real path).
         * Fall back to deriving from argv[0] for deeplink launches. */
        if (strncmp(url, "file://", 7) == 0) {
            strncpy(g_home_url, url, sizeof(g_home_url) - 1);
        } else {
            char argv0_buf[2048];
            strncpy(argv0_buf, argv[0], sizeof(argv0_buf) - 1);
            argv0_buf[sizeof(argv0_buf) - 1] = '\0';
            snprintf(g_home_url, sizeof(g_home_url),
                     "file://%s/home.html", dirname(argv0_buf));
        }
        g_home_url[sizeof(g_home_url) - 1] = '\0';
        chrome_set_home_callback(cb_home, view);
        fprintf(stderr, "[prism] home callback wired: %s\n", g_home_url);
    }
    if (chrome_set_blur_input_callback) {
        chrome_set_blur_input_callback(cb_blur_input, view);
        fprintf(stderr, "[prism] blur_input callback wired\n");
    }
    if (chrome_set_url)
        chrome_set_url(url);

    /* WebKit signals */
    g_signal_connect(view, "notify::title",                    G_CALLBACK(on_title_changed),    NULL);
    g_signal_connect(view, "notify::uri",                      G_CALLBACK(on_uri_changed),      NULL);
    g_signal_connect(view, "load-changed",                     G_CALLBACK(on_load_changed),     NULL);
    g_signal_connect(view, "load-failed",                      G_CALLBACK(on_load_failed),      NULL);
    g_signal_connect(view, "notify::estimated-load-progress",  G_CALLBACK(on_progress_changed), NULL);
    /* Intercept file:// navigations — WebKit blocks them from load_html() origin */
    g_signal_connect(view, "decide-policy",                    G_CALLBACK(on_decide_policy),    NULL);

    /* Initial page load — prefer session restore for normal (non-deeplink) launches */
    {
        const char *load_url = url;
        int is_home = (strncmp(url, "file://", 7) == 0);
        if (is_home) {
            const char *saved = session_load();
            if (saved) {
                fprintf(stderr, "[prism] session restore → %s\n", saved);
                load_url = saved;
            }
        }
        if (strncmp(load_url, "file://", 7) == 0)
            load_file_url(view, load_url);
        else
            webkit_web_view_load_uri(view, load_url);
        if (chrome_set_url) chrome_set_url(load_url);
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(view);
    PDL_Quit();
    return 0;
}
