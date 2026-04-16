# Prism Browser

A modern web browser for the **HP TouchPad** (webOS 3.0.5, Qualcomm APQ8060).

Built on WPE WebKit 2.38 with hardware H.264 video decode via the Qualcomm OMX stack. Self-contained — ships its own GStreamer, GLib, FFmpeg, and TLS stack; does not depend on the system's legacy GStreamer 0.10 or OpenSSL.

> **Windows 10 Mobile (Lumia 1520) port** — planned future target. WebKit WinCairo ARM32 build is in progress under `deps/`. Not the current focus.

---

## Status

### HP TouchPad (webOS 3.0.5) — Active

| Feature | Status |
|---|---|
| Web browsing (HTML5 / CSS3 / JS) | ✅ Working |
| HTTPS / TLS 1.2–1.3 | ✅ Working (GnuTLS 3.8, 2026 CA bundle) |
| MP4 / H.264 video streaming | ✅ Working |
| Qualcomm hardware H.264 decode | ✅ Working (`OMX.qcom.video.decoder.avc`) |
| WebM / MKV container | ✅ Demuxer present |
| AAC / MP3 audio decode | ✅ Decoded (silent — no audio output yet) |
| Audio output | ❌ No driver for GStreamer 1.x on webOS 3 |
| VP8 / VP9 / AV1 / Theora | ❌ Not yet wired up |
| Fullscreen video | ❌ Not yet |
| Bookmarks / history | ❌ Not yet |
| Downloads | ❌ Not yet |

### Windows 10 Mobile (Lumia 1520) — Planned

WebKit WinCairo ARM32 build in progress. Shell and on-device testing not started.

---

## Architecture

```
prism-browser (PDK shell)
  └── WPE WebKit 2.38
        ├── WPEWebProcess / WPENetworkProcess
        ├── WPE backend: libWPEBackend-sdl (SDL2 + EGL + PDK framebuffer)
        └── GStreamer 1.18.6 (media pipeline)
              ├── souphttpsrc   — HTTP/HTTPS streaming (libsoup)
              ├── gstisomp4     — MP4/M4V demuxer
              ├── gstmatroska   — WebM/MKV demuxer
              ├── gstpalmvideodec — Qualcomm OMX H.264 HW decoder
              │     └── libOmxCore.so (on-device, dlopen'd)
              │           → OMX.qcom.video.decoder.avc
              │           → NV12T tiled output → NV12 (inverted boustrophedon)
              └── libgstlibav   — FFmpeg software decode (AAC, MP3, fallback)
```

**Bundled dependencies** (cross-compiled for ARMv7 / glibc 2.8):
GLib 2.58 · ICU 67 · HarfBuzz · Cairo · FreeType · Fontconfig · libsoup · libxml2 · libepoxy · GnuTLS 3.8 · FFmpeg 6.1 · GStreamer 1.18.6 · libstdc++ 10

---

## Installation

Requires an HP TouchPad running webOS 3.0.5 connected via USB with the Palm SDK installed on your host machine.

**Download** the latest IPK from [Releases](https://github.com/Starkka15/prism-browser/releases).

```sh
# Palm SDK
palm-install com.prism.browser_1.0.0_all.ipk

# Or via novacom (no SDK required)
novacom put file:///tmp/prism.ipk < com.prism.browser_1.0.0_all.ipk
novacom run file:///usr/bin/palm-install -- /tmp/prism.ipk
```

Launch **Prism Browser** from the webOS launcher card stack.

---

## Building from Source

### Prerequisites

- Linux host with GCC 10 ARM cross-compiler (`arm-none-linux-gnueabi`)
- Palm SDK (`palm-package`) installed
- All dependencies pre-built into `webos/out/` (see build scripts below)

The cross-compiler is built by `~/webos-touchpad-modernize/` (crosstool-ng).

### Build the IPK

```sh
cd webos/
bash make-ipk.sh
# Produces: build/com.prism.browser_1.0.0_all.ipk
```

### Iterative development (fast push)

Rebuild and push individual components to a connected device without reinstalling the full IPK:

```sh
# Rebuild + push the OMX video decoder plugin
./fast-push.sh omx

# Push libWPEWebKit (already built)
./fast-push.sh webkit

# Push both
./fast-push.sh all
```

Requires `novacom` and a USB-connected TouchPad.

### Building dependencies

Each dependency has a build script in `webos/`:

```sh
bash build-gstreamer.sh
bash build-gst-plugins-base.sh
bash build-gst-plugins-good.sh
bash build-gst-plugins-bad.sh
bash build-gst-libav.sh
bash build-gst-palm-video-decoder.sh
bash build-webkit-webos.sh
# … etc.
```

### Building the OMX plugin only

```sh
cd webos/
export PATH="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin:$PATH"
PKG_CONFIG_PATH="out/lib/pkgconfig" ninja -C build/gst-palm-video-decoder
```

---

## Qualcomm NV12T Decoder Notes

The TouchPad's Qualcomm APQ8060 OMX decoder outputs `QOMX_COLOR_FormatYUV420PackedSemiPlanar32m` — a tiled NV12 format using 64×32 pixel tiles in **inverted boustrophedon** order.

Key implementation details in `webos/gst-palm-video-decoder/src/gstpalmvideodec.c`:

- Tile order: even column-pairs → even row first; odd column-pairs → odd row first (same formula for Y and UV planes)
- Buffer is **packed** — invalid boustrophedon positions (ty ≥ tiles_h) are omitted; a separate `buf_idx` tracks the actual buffer read pointer
- `tiles_h_y = ceil(H / 32)`, `tiles_h_uv = ceil((H/2) / 32)` — computed directly from display dimensions
- Output strides come from `GST_VIDEO_FRAME_PLANE_STRIDE` (GStreamer may align beyond width)

---

## Device Notes

- `/dev/pmem_adsp` and `/dev/msm_vidc_dec` must be `chmod a+rw` before the OMX decoder initialises — the launcher script handles this
- GStreamer registry is cached at `/tmp/prism-gst-registry.bin`; deleted on each OMX plugin update
- Video and temp cache stored under `/media/internal/prism/` (avoids filling the 40 MB `/tmp`)
- `libgoodabort.so` and `libmemcpy.so` are preloaded system-wide by `/etc/ld.so.preload` — expected, not a bug
