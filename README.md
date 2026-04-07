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

### Windows 10 Mobile (Lumia 1520)
- [x] WebKit WinCairo ARM32 build (ninja, MSVC cross-compiler)
- [x] Dependency chain (cairo, freetype, harfbuzz, pixman, curl, icu, …)
- [ ] Prism UWP shell — compiling
- [ ] On-device test (Lumia 1520)

### HP TouchPad / webOS 3.0.5 (see `webos/`)
- [x] Full dependency chain cross-compiled for ARMv7 / glibc 2.8
- [x] WPE WebKit 2.38 built and linked
- [x] SDL 1.2 + PDK EGL WPE backend (`wpe-backend-sdl`)
- [x] `prism-browser` shell (GLib main loop, WPE view)
- [x] IPK packaging via `palm-package`; installs via `palm-install`
- [ ] **Blocked**: `libstdc++.so.6` UNIQUE symbol ABI issue (glibc 2.8 predates `STB_GNU_UNIQUE`)
- [ ] EGL/WebProcess launch on-device
- [ ] Visible rendering

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

### Windows 10 Mobile
- The HWND-child approach in UWP may not composite correctly under W10M's shell.
  If rendering is blank on-device, the fallback plan is XAML + `SwapChainPanel`.
- `WebKit.resources` are not bundled in the AppX; deploy them alongside the package
  if localisation strings or the Web Inspector are needed.

### webOS / HP TouchPad

**Current blocker**: GCC 10's `libstdc++.so.6.0.28` uses `STB_GNU_UNIQUE` symbol
binding for C++ locale objects (e.g. `std::numpunct<char>::id`). The device's
`ld.so` (glibc 2.8) pre-dates `STB_GNU_UNIQUE` (added in glibc 2.11) and fails
to resolve these symbols at load time.

**Fix needed**: Rebuild `libstdc++` with `-fno-gnu-unique` (requires recompiling
GCC 10 for `arm-none-linux-gnueabi` with `--disable-gnu-unique-object`), or
binary-patch `STB_GNU_UNIQUE → STB_GLOBAL` and also strip version tags from the
affected `.dynsym` entries so glibc 2.8's linker can match them.

**Other notes**:
- IPK must be uploaded to `/media/internal/.developer/` before `palm-install`
  will pick it up — create that directory once via `novacom run file:///bin/mkdir -- -p /media/internal/.developer`
- Symlinks cannot be created manually on `cryptofs` (`Operation not permitted`);
  they must be included in the IPK via `palm-package`
- `LD_LIBRARY_PATH` is set by the `prism` wrapper script relative to `$0`
- System `libgoodabort.so` and `libmemcpy.so` are preloaded by `/etc/ld.so.preload`
- `libgcc_s.so.1` is not bundled — falls through to the system copy in `/usr/lib`
