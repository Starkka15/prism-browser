# Prism Browser — webOS TouchPad Port: Reference Doc

## What This Is

A standalone WPE WebKit 2.38 browser for the HP TouchPad (webOS 3.0.5, Adreno 220 GPU,
ARMv7 Cortex-A9, 1024×768). Not a webOS app in the SDK sense — it's a native PDK app
that runs WebKit in-process via WPE and draws its own chrome bar using SDL 1.2 + GLES2.

**Repo:** `Starkka15/prism-browser` (branch: master, webos/ subdirectory)  
**Last commit:** `e95921c`

---

## Hardware & Platform Notes

- **GPU:** Qualcomm Adreno 220 (NOT PowerVR — the PDK docs are misleading)
- **EGL vendor:** `Qualcomm Inc`
- **Runtime EGL extensions:** `EGL_KHR_image EGL_QUALCOMM_shared_image EGL_AMD_create_image EGL_KHR_lock_surface EGL_KHR_lock_surface2 EGL_KHR_fence_sync`
- **No** `EGL_MESA_drm_image` at runtime (header has it; driver doesn't implement it)
- **EGL display:** `EGL_DEFAULT_DISPLAY` only — no platform extension
- **NativeWindowType=0** → fullscreen PDK surface, **write-only** (glReadPixels returns GL_INVALID_VALUE on it)
- **PDK cross-compiler:** `arm-none-linux-gnueabi-gcc` (CodeSourcery GCC 4.5.2, at `/opt/PalmPDK/arm-gcc/bin/`)
- **Must use** `-std=gnu99` (or CMake `set(CMAKE_C_STANDARD 99)`) — compiler defaults to C89
- **Three processes at runtime:** prism-browser (UI), WPENetworkProcess, WPEWebProcess

---

## Architecture

```
prism-browser (UI process)
  ├── SDL 1.2 → SDL_SetVideoMode(SDL_OPENGL) → EGL ctx=0x1 on the VISIBLE surface
  ├── GLES2 compositor: renders chrome bar + WebKit pixels → SDL_GL_SwapBuffers()
  ├── libWPEBackend-sdl.so (RTLD_GLOBAL) — provides WPE view backend
  └── socketpair IPC to WPEWebProcess

WPEWebProcess
  ├── WebKit renders to EGL PBuffer (ctx=0x2, surf=0x3) — readable
  ├── frame_will_render: eglMakeCurrent → PBuffer
  ├── frame_rendered: fence sync → lock surface (or glReadPixels fallback)
  │     → memcpy → /tmp/prism-fb (mmap'd shm, RGBA8, 1024×704×4 = 2.8MB)
  └── sends 'F' byte over socket after each frame
```

### IPC Socket
- `socketpair(AF_UNIX, SOCK_STREAM)` created in `view_backend_create`
- UI holds `host_fd` (reads 'F' frames)
- WPEWebProcess holds `renderer_fd` / target `fd` (writes 'F' frames)
- Socket is `SOCK_STREAM` — drain it fully in `on_sdl_poll` to prevent backpressure freeze

### Shared Memory
- Path: `/tmp/prism-fb`
- WPEWebProcess: `O_CREAT|O_RDWR`, `ftruncate`, `mmap(PROT_READ|PROT_WRITE)`
- UI process: `O_RDONLY`, `mmap(PROT_READ)` — opened lazily in `repaint()`
- Size: `1024 × 704 × 4` bytes (WebKit area only, chrome bar excluded)

---

## Screen Layout

```
y=0    ┌──────────────────────────┐
       │  Chrome bar  (64px)      │  cairo ARGB32 → BGRA-swap shader
y=64   ├──────────────────────────┤
       │                          │
       │  WebKit content (704px)  │  RGBA from PBuffer → straight shader
       │                          │
y=768  └──────────────────────────┘
```

- `WEBOS_SCREEN_W = 1024`, `WEBOS_SCREEN_H = 768`, `CHROME_H = 64`, `WEBKIT_H = 704`
- All constants in `wpe-backend-sdl/src/view-backend.h`

---

## Key Files

| File | Process | Role |
|------|---------|------|
| `wpe-backend-sdl/src/backend-egl.c` | WPEWebProcess | PBuffer creation, frame capture (fence sync + lock surface) |
| `wpe-backend-sdl/src/backend-egl.h` | WPEWebProcess | sdl_target / sdl_backend structs + EGL ext typedefs |
| `wpe-backend-sdl/src/view-backend.c` | UI | GLES2 compositor, SDL event loop, chrome toolbar, repaint loop |
| `wpe-backend-sdl/src/view-backend.h` | UI | sdl_view_backend struct + layout constants |
| `wpe-backend-sdl/src/loader.c` | both | WPE backend entry point (registers interfaces) |
| `wpe-backend-sdl/src/renderer-host.c` | UI | WPE renderer host process glue |
| `prism-browser/main.c` | UI | PDL_Init, wpe_loader_init, WebKit setup, PrismIme |
| `make-ipk.sh` | — | Packages everything into .ipk with all bundled .so libs |

---

## Rendering Pipeline Detail

### WPEWebProcess side (`backend-egl.c`)

**`frame_will_render`:**
1. `ensure_pbuffer()` — on first call, queries EGL config, creates PBuffer, loads extension function pointers
2. `eglMakeCurrent(pb_dpy, pb_surf, pb_surf, pb_ctx)` — redirect WebKit's draw target to PBuffer

**`frame_rendered`:**
1. Ensure PBuffer is current (re-assert if WPE changed it)
2. **Fence sync** (`EGL_KHR_fence_sync`): `eglCreateSyncKHR(FENCE)` → `eglClientWaitSyncKHR(EGL_FOREVER_KHR)` — explicit GPU completion, avoids implicit stall
3. **Fast path** (if `has_lock == 1`): `eglLockSurfaceKHR` → query `EGL_BITMAP_POINTER_KHR` + `EGL_BITMAP_PITCH_KHR` + `EGL_BITMAP_ORIGIN_KHR` → memcpy to shm → `eglUnlockSurfaceKHR`
   - `EGL_LOWER_LEFT_KHR`: same byte order as glReadPixels → straight copy
   - `EGL_UPPER_LEFT_KHR`: top-down → flip rows to match glReadPixels convention
4. **Fallback** (if `has_lock == 0` or lock fails): `glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)`
5. Restore window surface: `eglMakeCurrent(pb_dpy, win_surf, win_surf, pb_ctx)`
6. `wpe_renderer_backend_egl_target_dispatch_frame_complete()`
7. Write `'F'` to socket

**`has_lock` is set** if `EGL_LOCK_SURFACE_BIT_KHR` is in the PBuffer config's `EGL_SURFACE_TYPE`. Checked in `ensure_pbuffer`. First-frame log says: `PBuffer surface created: ... has_lock=0/1`

### UI process side (`view-backend.c`)

**`on_sdl_poll`** (every 16ms, `g_timeout_add(16, ...)`):
1. Drain socket: `recv(MSG_DONTWAIT)` loop → `dispatch_frame_displayed()` for each 'F', set `webkit_dirty = 1`
2. `pump_sdl_events()` — SDL input, dispatch to WebKit or chrome handler
3. `repaint()` — always called every tick

**`repaint()`:**
1. If `webkit_dirty`: `glTexImage2D(blit_tex, RGBA, fb_pixels)` → clear `webkit_dirty`
2. `draw_quad(blit_prog, blit_tex, ...)` — WebKit area (straight RGBA)
3. If `chrome_dirty`: `chrome_render()` → `glTexImage2D(chrome_tex, RGBA, chrome_pixels)`
4. `draw_quad(cairo_prog, chrome_tex, ...)` — Chrome bar (BGRA-swap shader for cairo)
5. `SDL_GL_SwapBuffers()`

**Critical:** `SDL_GL_SwapBuffers()` must only be called from `on_sdl_poll` (the 16ms timer). Calling it from a GIO watch callback causes a hang on this driver.

---

## Input Handling

### Keyboard
- **URL bar:** tap → `PDL_SetKeyboardState(PDL_TRUE)`. SDL delivers `SDL_KEYDOWN` events. Unicode synthesized from `keysym.sym + KMOD_SHIFT` when `keysym.unicode == 0` (PDL keyboard doesn't always populate it).
- **Web content:** `PrismIme` — a `WebKitInputMethodContext` GObject subclass in `main.c`. `notify_focus_in` → `PDL_SetKeyboardState(PDL_TRUE)`, `notify_focus_out` → `PDL_FALSE`. SDL key events dispatched to WebKit via `wpe_view_backend_dispatch_keyboard_event()`.
- **No cairo/custom keyboard** — fully replaced with native webOS PDL keyboard.

### Touch
- SDL 1.2 PDK maps single touch to `SDL_MOUSEBUTTONDOWN/MOTION/UP`
- Touch on chrome bar (y < 64): handled by `chrome_handle_touch()`
- Touch on WebKit area (y >= 64): dispatched as `wpe_input_touch_event` with `y -= CHROME_H`
- Scroll threshold: 16px movement → `touch_was_scroll = 1`
- Multi-touch: not yet implemented (needs PDL raw touch events)

---

## TLS / HTTPS

Bundled in IPK (all built from source for ARMv7):
- GMP 6.3.0, Nettle 3.9.1, libtasn1 4.19.0, GnuTLS 3.8.3, glib-networking 2.74.0
- Mozilla CA bundle → copied to `/tmp/prism-ca.crt` on launch via wrapper script
- GnuTLS built with `--with-default-trust-store-file=/tmp/prism-ca.crt`

---

## Build

```bash
cd ~/prism-browser/webos

# Build all deps (one-time, already done):
bash build-libwpe.sh
# ... (cairo, glib, webkit, etc.)

# Build backend + browser:
bash build-wpe-backend-sdl.sh
bash build-prism-browser.sh

# Package:
bash make-ipk.sh
# → build/com.prism.browser_1.0.0_all.ipk

# Install on device (TouchPad via USB novacom):
palm-install build/com.prism.browser_1.0.0_all.ipk
```

**CMake toolchain:** `toolchain.cmake` (sets `arm-none-linux-gnueabi-*` tools, PDK sysroot)  
**Output prefix:** `out/` (headers + libs used by subsequent build steps)  
**IPK sysroot:** `deploy/` (populated by make-ipk.sh from out/ + PDK device libs)

---

## Critical PDK Quirks

1. **Never call `SDL_Init(SDL_INIT_VIDEO)` in WPEWebProcess.** SysMgr will steal display ownership from the UI process → black screen. WPEWebProcess only needs EGL directly.

2. **EGL window surface (NativeWindowType=0) is write-only.** `glReadPixels` on it returns `GL_INVALID_VALUE`. Must redirect to PBuffer for readback.

3. **`eglMakeCurrent` with already-current context hangs** on this Qualcomm PDK driver. Don't call it redundantly. The UI process SDL EGL context stays current for the entire process lifetime.

4. **`SDL_GL_SwapBuffers` is context-sensitive** — only reliable from the 16ms timer callback, not from GIO watch callbacks.

5. **Socket backpressure:** if the UI doesn't drain the socket, WPEWebProcess blocks on `write()`, stops rendering, screen freezes. Always drain fully with `recv(MSG_DONTWAIT)` in a loop.

6. **PDL must be initialized before SDL:** `PDL_Init(0)` before `SDL_Init()`.

---

## Planned Next Steps (in priority order)

1. **Test EGL lock surface** — first IPK with `e95921c` changes; check `has_lock=0/1` in logread
2. **Multi-touch** — pinch-zoom, two-finger scroll via PDL raw touch events
3. **Screen rotation** — landscape ↔ portrait
4. **Bookmarks / Favorites** — store + display saved URLs
5. **Downloads** — WebKit download API + progress UI
6. **Smooth scrolling** — currently disabled in WebKit settings; may enable once perf is confirmed

---

## Diagnostics

Connect via novacom USB and run:
```bash
# Live logs from the browser:
palm-log com.prism.browser

# Or:
logread -f | grep prism
```

Key log lines to check on startup:
```
[wpe-backend-sdl] EGL exts: FenceSync=YES LockSurface=YES
[wpe-backend-sdl] PBuffer surface created: 0x... (1024x704) has_lock=0/1
[wpe-backend-sdl] lock readback OK: origin=... pitch=4096 center=(r,g,b,a)
```
