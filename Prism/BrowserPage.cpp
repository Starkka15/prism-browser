#include "pch.h"
#include "BrowserPage.h"

#include <WebKit/WKFramePolicyListener.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKNavigationResponseRef.h>
#include <WebKit/WKError.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

BrowserPage::~BrowserPage()
{
    if (m_urlBarHwnd)
        DestroyWindow(m_urlBarHwnd);
    // WKRetainPtr destructors release m_view, m_pageConfig, m_context
}

bool BrowserPage::init(HWND parentHwnd, int width, int height)
{
    m_parentHwnd = parentHwnd;
    m_width  = width;
    m_height = height;

    // ── WebKit context ──────────────────────────────────────────────────────
    auto contextConfig = adoptWK(WKContextConfigurationCreate());
    m_context = adoptWK(WKContextCreateWithConfiguration(contextConfig.get()));

    // ── Page configuration ──────────────────────────────────────────────────
    m_pageConfig = adoptWK(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(m_pageConfig.get(), m_context.get());

    auto prefs = adoptWK(WKPreferencesCreate());
    WKPreferencesSetJavaScriptEnabled(prefs.get(), true);
    WKPreferencesSetLoadsImagesAutomatically(prefs.get(), true);
    WKPageConfigurationSetPreferences(m_pageConfig.get(), prefs.get());

    // ── WKView ──────────────────────────────────────────────────────────────
    RECT viewRect { 0, kURLBarHeight, width, height };
    m_view = adoptWK(WKViewCreate(viewRect, m_pageConfig.get(), parentHwnd));
    if (!m_view)
        return false;
    WKViewSetIsInWindow(m_view.get(), true);

    WKPageRef page = WKViewGetPage(m_view.get());

    // ── Navigation client ───────────────────────────────────────────────────
    WKPageNavigationClientV0 navClient {};
    navClient.base.version    = 0;
    navClient.base.clientInfo = this;
    navClient.didStartProvisionalNavigation    = didStartProvisionalNavigation;
    navClient.didCommitNavigation              = didCommitNavigation;
    navClient.didFinishNavigation              = didFinishNavigation;
    navClient.didFailNavigation                = didFailNavigation;
    navClient.didFailProvisionalNavigation     = didFailProvisionalNavigation;
    navClient.decidePolicyForNavigationAction  = decidePolicyForNavigationAction;
    navClient.decidePolicyForNavigationResponse = decidePolicyForNavigationResponse;
    WKPageSetPageNavigationClient(page, &navClient.base);

    // ── URL bar (Win32 EDIT child) ──────────────────────────────────────────
    m_urlBarHwnd = CreateWindowEx(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, width, kURLBarHeight,
        parentHwnd, reinterpret_cast<HMENU>(IDC_URL_BAR),
        GetModuleHandle(nullptr), nullptr);

    if (!m_urlBarHwnd)
        return false;

    // Set a reasonable font
    HFONT font = CreateFontW(
        24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    if (font)
        SendMessage(m_urlBarHwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    // Subclass the EDIT to catch VK_RETURN
    SetWindowSubclass(m_urlBarHwnd, urlBarSubclassProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));

    // Navigate to start page
    navigate(L"https://www.google.com");
    return true;
}

void BrowserPage::resize(int width, int height)
{
    m_width  = width;
    m_height = height;

    if (m_urlBarHwnd)
        MoveWindow(m_urlBarHwnd, 0, 0, width, kURLBarHeight, TRUE);

    if (m_view) {
        HWND viewHwnd = WKViewGetWindow(m_view.get());
        if (viewHwnd)
            MoveWindow(viewHwnd, 0, kURLBarHeight, width, height - kURLBarHeight, TRUE);
    }
}

void BrowserPage::navigate(const std::wstring& rawURL)
{
    if (!m_view)
        return;

    // Prefix bare hostnames with https://
    std::wstring url = rawURL;
    if (url.find(L"://") == std::wstring::npos)
        url = L"https://" + url;

    auto wkURL = adoptWK(WKURLCreateWithUTF8CString(
        [&] {
            int n = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, utf8.data(), n, nullptr, nullptr);
            return utf8;
        }().c_str()
    ));

    WKPageLoadURL(WKViewGetPage(m_view.get()), wkURL.get());
    updateURLBar(url);
}

void BrowserPage::focusURLBar()
{
    if (m_urlBarHwnd) {
        SetFocus(m_urlBarHwnd);
        SendMessage(m_urlBarHwnd, EM_SETSEL, 0, -1);
    }
}

void BrowserPage::onURLBarReturn()
{
    if (!m_urlBarHwnd)
        return;
    int len = GetWindowTextLength(m_urlBarHwnd);
    if (len <= 0)
        return;
    std::wstring text(len, L'\0');
    GetWindowText(m_urlBarHwnd, text.data(), len + 1);
    navigate(text);
    // Return focus to the web view so the page receives keyboard input
    if (m_view) {
        HWND vhwnd = WKViewGetWindow(m_view.get());
        if (vhwnd)
            SetFocus(vhwnd);
    }
}

HWND BrowserPage::viewHwnd() const
{
    return m_view ? WKViewGetWindow(m_view.get()) : nullptr;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void BrowserPage::updateURLBar(const std::wstring& url)
{
    if (m_urlBarHwnd)
        SetWindowText(m_urlBarHwnd, url.c_str());
}

// ---------------------------------------------------------------------------
// URL bar subclass proc — catches Enter key
// ---------------------------------------------------------------------------

LRESULT CALLBACK BrowserPage::urlBarSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subclassId*/, DWORD_PTR refData)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        auto* self = reinterpret_cast<BrowserPage*>(refData);
        if (self)
            self->onURLBarReturn();
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WebKit navigation callbacks
// ---------------------------------------------------------------------------

void BrowserPage::didStartProvisionalNavigation(
    WKPageRef page, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* self = static_cast<const BrowserPage*>(info);
    if (!self || !self->m_urlBarHwnd)
        return;
    auto wkURL = adoptWK(WKPageCopyActiveURL(page));
    if (!wkURL)
        return;
    auto wkStr = adoptWK(WKURLCopyString(wkURL.get()));
    auto url = wkStringToWide(wkStr.get());
    SetWindowText(self->m_urlBarHwnd, url.c_str());
}

void BrowserPage::didCommitNavigation(
    WKPageRef page, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* self = static_cast<const BrowserPage*>(info);
    if (!self || !self->m_urlBarHwnd)
        return;
    auto wkURL = adoptWK(WKPageCopyActiveURL(page));
    if (!wkURL)
        return;
    auto wkStr = adoptWK(WKURLCopyString(wkURL.get()));
    SetWindowText(self->m_urlBarHwnd, wkStringToWide(wkStr.get()).c_str());
}

void BrowserPage::didFinishNavigation(
    WKPageRef, WKNavigationRef, WKTypeRef, const void*)
{
    // Could update back/forward button state here if we add them
}

void BrowserPage::didFailNavigation(
    WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void*)
{
    // Could show error page
}

void BrowserPage::didFailProvisionalNavigation(
    WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void*)
{
    // Could show error page
}

void BrowserPage::decidePolicyForNavigationAction(
    WKPageRef, WKNavigationActionRef, WKFramePolicyListenerRef listener,
    WKTypeRef, const void*)
{
    WKFramePolicyListenerUse(listener);
}

void BrowserPage::decidePolicyForNavigationResponse(
    WKPageRef, WKNavigationResponseRef, WKFramePolicyListenerRef listener,
    WKTypeRef, const void*)
{
    WKFramePolicyListenerUse(listener);
}
