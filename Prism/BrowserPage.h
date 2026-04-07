#pragma once
#include "pch.h"

// ---------------------------------------------------------------------------
// Minimal RAII wrapper for WebKit C API objects.
// Replaces WKPtr<> which pulls in WTF internals unavailable outside the
// WebKit source tree.
// ---------------------------------------------------------------------------
template<typename T>
class WKPtr {
public:
    WKPtr() = default;
    explicit WKPtr(T p) : m_ptr(p) {}
    ~WKPtr() { if (m_ptr) WKRelease(reinterpret_cast<WKTypeRef>(m_ptr)); }
    WKPtr(const WKPtr&) = delete;
    WKPtr& operator=(const WKPtr&) = delete;
    WKPtr(WKPtr&& o) noexcept : m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    WKPtr& operator=(WKPtr&& o) noexcept {
        if (m_ptr) WKRelease(reinterpret_cast<WKTypeRef>(m_ptr));
        m_ptr = o.m_ptr; o.m_ptr = nullptr; return *this;
    }
    T get() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }
private:
    T m_ptr { nullptr };
};

// Adopt a WK object that was just created (already has a +1 retain count).
template<typename T>
WKPtr<T> adoptWK(T p) { return WKPtr<T>(p); }

// ---------------------------------------------------------------------------

// Height of the URL bar chrome at top of window (logical pixels)
static constexpr int kURLBarHeight = 48;
// ID used to identify URL bar EDIT control in WM_COMMAND
static constexpr int IDC_URL_BAR = 1001;

// Helpers: convert between WKString and std::wstring
inline std::wstring wkStringToWide(WKStringRef s) {
    size_t len = WKStringGetLength(s);
    std::wstring result(len, L'\0');
    WKStringGetCharacters(s, reinterpret_cast<WKChar*>(result.data()), len);
    return result;
}

inline WKPtr<WKStringRef> wideToWKString(const std::wstring& s) {
    // WKStringCreateWithUTF8CString requires UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, utf8.data(), utf8Len, nullptr, nullptr);
    return adoptWK(WKStringCreateWithUTF8CString(utf8.c_str()));
}

// The main browser controller. Owns WKContext, WKView, and the URL bar HWND.
// Attach it to a parent HWND (the CoreWindow's HWND).
class BrowserPage {
public:
    BrowserPage() = default;
    ~BrowserPage();

    // Call once after the CoreWindow HWND is available.
    bool init(HWND parentHwnd, int width, int height);

    // Resize to fill parent (called on WM_SIZE / CoreWindow SizeChanged).
    void resize(int width, int height);

    // Navigate to an absolute URL string (UTF-16).
    void navigate(const std::wstring& url);

    // Focus the URL bar so the user can type.
    void focusURLBar();

    // WebKit callbacks (static, forwarded to instance via clientInfo)
    static void didStartProvisionalNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* info);
    static void didCommitNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* info);
    static void didFinishNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* info);
    static void didFailNavigation(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void* info);
    static void didFailProvisionalNavigation(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void* info);
    static void decidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef, WKFramePolicyListenerRef,
                                                WKTypeRef, const void* info);
    static void decidePolicyForNavigationResponse(WKPageRef, WKNavigationResponseRef, WKFramePolicyListenerRef,
                                                  WKTypeRef, const void* info);

    // Called by the parent WndProc when the URL bar sends WM_COMMAND/EN_RETURN
    void onURLBarReturn();

    // SubclassProc for URL bar EDIT (catches Enter key)
    static LRESULT CALLBACK urlBarSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND viewHwnd() const;

private:
    void updateURLBar(const std::wstring& url);

    HWND m_parentHwnd { nullptr };
    HWND m_urlBarHwnd { nullptr };

    WKPtr<WKContextRef>           m_context;
    WKPtr<WKPageConfigurationRef> m_pageConfig;
    WKPtr<WKViewRef>              m_view;

    int m_width { 0 };
    int m_height { 0 };
};
