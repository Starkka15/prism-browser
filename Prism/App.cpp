#include "pch.h"
#include "BrowserPage.h"

namespace wrl  = Microsoft::WRL;
namespace wrlw = Microsoft::WRL::Wrappers;
namespace ac   = ABI::Windows::ApplicationModel::Core;
namespace uc   = ABI::Windows::UI::Core;
namespace wf   = ABI::Windows::Foundation;

// ---------------------------------------------------------------------------
// Parent window class for WebKit (hosts URL bar + WKView as children)
// ---------------------------------------------------------------------------

static constexpr wchar_t kParentClassName[] = L"PrismHostWindow";

static std::unique_ptr<BrowserPage> g_browser;

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        if (g_browser)
            g_browser->resize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_SETFOCUS:
        if (g_browser) {
            HWND vhwnd = g_browser->viewHwnd();
            if (vhwnd) SetFocus(vhwnd);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void RegisterHostClass()
{
    WNDCLASSEX wc {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kParentClassName;
    RegisterClassEx(&wc);
}

// ---------------------------------------------------------------------------
// IFrameworkView implementation (WRL)
// ---------------------------------------------------------------------------

class PrismView : public wrl::RuntimeClass<
    wrl::RuntimeClassFlags<wrl::WinRtClassicComMix>,
    ac::IFrameworkView>
{
    InspectableClass(L"Prism.PrismView", BaseTrust)

public:
    HRESULT STDMETHODCALLTYPE Initialize(ac::ICoreApplicationView*) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetWindow(uc::ICoreWindow* window) override
    {
        m_window = window;

        // Get the underlying HWND from the CoreWindow
        wrl::ComPtr<ICoreWindowInterop> interop;
        HRESULT hr = window->QueryInterface(IID_PPV_ARGS(&interop));
        if (FAILED(hr)) return hr;
        hr = interop->get_WindowHandle(&m_coreHwnd);
        if (FAILED(hr)) return hr;

        // Wire up SizeChanged
        EventRegistrationToken token;
        window->add_SizeChanged(
            wrl::Callback<wf::ITypedEventHandler<uc::CoreWindow*, uc::WindowSizeChangedEventArgs*>>(
                [this](uc::ICoreWindow*, uc::IWindowSizeChangedEventArgs* args) -> HRESULT {
                    wf::Size size;
                    args->get_Size(&size);
                    int w = static_cast<int>(size.Width);
                    int h = static_cast<int>(size.Height);
                    if (m_hostHwnd) MoveWindow(m_hostHwnd, 0, 0, w, h, TRUE);
                    if (g_browser) g_browser->resize(w, h);
                    return S_OK;
                }).Get(),
            &token);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Load(HSTRING) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Run() override
    {
        m_window->Activate();

        wf::Rect bounds;
        m_window->get_Bounds(&bounds);
        int w = static_cast<int>(bounds.Width);
        int h = static_cast<int>(bounds.Height);

        // Create host HWND parented to the CoreWindow HWND
        RegisterHostClass();
        m_hostHwnd = CreateWindowEx(
            WS_EX_NOPARENTNOTIFY,
            kParentClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, w, h,
            m_coreHwnd, nullptr,
            GetModuleHandle(nullptr), nullptr);

        g_browser = std::make_unique<BrowserPage>();
        g_browser->init(m_hostHwnd, w, h);

        // Combined CoreWindow + Win32 message pump
        wrl::ComPtr<uc::ICoreDispatcher> dispatcher;
        m_window->get_Dispatcher(&dispatcher);

        while (true) {
            dispatcher->ProcessEvents(uc::CoreProcessEventsOption_ProcessAllIfPresent);

            MSG msg {};
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return S_OK;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Sleep(1);
        }
    }

    HRESULT STDMETHODCALLTYPE Uninitialize() override
    {
        g_browser.reset();
        return S_OK;
    }

private:
    wrl::ComPtr<uc::ICoreWindow> m_window;
    HWND m_coreHwnd { nullptr };
    HWND m_hostHwnd { nullptr };
};

// ---------------------------------------------------------------------------
// IFrameworkViewSource implementation (WRL)
// ---------------------------------------------------------------------------

class PrismViewSource : public wrl::RuntimeClass<
    wrl::RuntimeClassFlags<wrl::WinRtClassicComMix>,
    ac::IFrameworkViewSource>
{
    InspectableClass(L"Prism.PrismViewSource", BaseTrust)

public:
    HRESULT STDMETHODCALLTYPE CreateView(ac::IFrameworkView** view) override
    {
        auto v = wrl::Make<PrismView>();
        return v.CopyTo(view);
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    RoInitialize(RO_INIT_MULTITHREADED);

    wrl::ComPtr<ac::ICoreApplication> app;
    HRESULT hr = RoGetActivationFactory(
        wrlw::HStringReference(L"Windows.ApplicationModel.Core.CoreApplication").Get(),
        IID_PPV_ARGS(&app));

    if (SUCCEEDED(hr)) {
        auto source = wrl::Make<PrismViewSource>();
        app->Run(source.Get());
    }

    RoUninitialize();
    return 0;
}
