# Prism Browser

**WIP — currently in the building phase.**

A WebKit-based web browser for Windows 10 Mobile (Lumia 1520), targeting ARM32.

## Architecture

- **Shell**: UWP app (`IFrameworkView` via WRL, no XAML) — Win32 HWND hosted inside the CoreWindow
- **Renderer**: WebKit WinCairo, C API (`WKView`, `WKPage`, `WKContext`)
- **URL bar**: Win32 `EDIT` control (Segoe UI 24pt, VK_RETURN to navigate)
- **Target device**: Nokia Lumia 1520 (ARM32, Windows 10 Mobile)
- **Min OS**: 10.0.10240.0 (W10M RTM), built against SDK 10.0.16299.0

## Status

- [x] WebKit WinCairo ARM32 build (ninja, MSVC cross-compiler)
- [x] Dependency chain (cairo, freetype, harfbuzz, pixman, curl, icu, …)
- [ ] Prism UWP shell — compiling
- [ ] On-device test (Lumia 1520)

## Build

### Prerequisites

- Windows 10 host (or VM) with Visual Studio 2022 + ARM build tools
- WebKit WinCairo ARM32 already built (see `deps/`)

### Shell

```powershell
.\build-prism.ps1
```

Produces `Prism\Release\ARM\Prism.appx` — sideload via Windows Device Portal.

### WebKit deps

See `deps/BUILD-ORDER.md` and the individual `deps/build-*.ps1` scripts.

## Notes

- The HWND-child approach in UWP may not composite correctly under W10M's shell.
  If rendering is blank on-device, the fallback plan is XAML + `SwapChainPanel`.
- `WebKit.resources` are not bundled in the AppX; deploy them alongside the package
  if localisation strings or the Web Inspector are needed.
