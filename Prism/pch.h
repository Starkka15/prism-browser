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

// Standard library
#include <string>
#include <memory>
#include <functional>
#include <cassert>

// WebKit C API
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
#include <WebKit/WKRetainPtr.h>
