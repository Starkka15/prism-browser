#pragma once

// Windows + UWP
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

// CoreWindow → HWND interop (ICoreWindowInterop)
#include <CoreWindow.h>

// WinRT infrastructure via WRL (available in SDK 10.0.16299.0)
#include <roapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

// WinRT ABI type headers
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.foundation.h>

// MSVC 14.44 regression: <cstdlib> does unconditional `using ::getenv/system`
// even under WINAPI_FAMILY_APP where stdlib.h omits those declarations.
// Provide dead stubs in the global C namespace so the using-declarations resolve.
// These are never called — AppContainer code has no path to them.
#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
extern "C" {
    inline char* getenv(const char*) noexcept { return nullptr; }
    inline int   system(const char*) noexcept { return -1; }
}
#endif

// Standard library
#include <string>
#include <memory>
#include <cassert>

// WebKit C API (public headers only — WKRetainPtr.h is intentionally excluded
// because it pulls in WTF internals not present in our installed headers)
#include <WebKit/WKBase.h>
#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKPreferencesRef.h>
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKView.h>
