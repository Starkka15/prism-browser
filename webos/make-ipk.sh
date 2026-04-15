#!/usr/bin/env bash
# Build Prism Browser IPK for HP TouchPad (webOS 3.0.5)
# Uses palm-package so palm-install works over novacom.
# Everything lives in /usr/palm/applications/com.prism.browser/
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$SCRIPT_DIR/out"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
STRIP="$GCC10/arm-none-linux-gnueabi-strip"
APP_ID="com.prism.browser"
APPDIR="$SCRIPT_DIR/build/ipk-app/$APP_ID"
OUTDIR="$SCRIPT_DIR/build"

echo "=== Prism Browser IPK builder ==="

# ── 1. Prepare app directory ───────────────────────────────────────────────
rm -rf "$APPDIR" && mkdir -p "$APPDIR/lib" "$APPDIR/lib/gio/modules" "$APPDIR/lib/gstreamer-1.0" "$APPDIR/libexec" "$APPDIR/etc/fonts" "$APPDIR/certs"

# Copy fontconfig config (searches device system fonts + temp cache)
cp "$SCRIPT_DIR/bundle-extras/etc/fonts/fonts.conf" "$APPDIR/etc/fonts/"

# CA certificate bundle (GnuTLS looks for /tmp/prism-ca.crt; wrapper copies it there)
cp /etc/ssl/certs/ca-certificates.crt "$APPDIR/certs/ca-certificates.crt"

# ── 2. appinfo.json ────────────────────────────────────────────────────────
cat > "$APPDIR/appinfo.json" << APPINFO
{
    "id": "$APP_ID",
    "version": "1.0.0",
    "type": "pdk",
    "title": "Prism Browser",
    "vendor": "Prism Project",
    "icon": "icon.png",
    "main": "prism"
}
APPINFO

# ── 3. Wrapper launch script (main entry point) ────────────────────────────
# webOS sets HOME to the app dir; we derive our path from $0
cat > "$APPDIR/prism" << 'WRAPPER'
#!/bin/sh
# Prism Browser launcher

# Resolve our install directory.
# $0 may be an absolute path (SysMgr), a relative path, or just the filename.
# We try $0 first; if readlink -f is unavailable or gives a bad result,
# webOS SysMgr sets HOME to the app directory for PDK apps — use that fallback.
_self="$0"
if [ "${_self#/}" = "$_self" ]; then
    # not absolute — make it relative to CWD
    _self="$PWD/$_self"
fi
D=$(dirname "$_self")
# If the binary isn't where we expect, trust $HOME (set by SysMgr to app dir)
if [ ! -x "$D/prism-browser" ] && [ -x "$HOME/prism-browser" ]; then
    D="$HOME"
fi

# Log to /tmp/prism.log so we can inspect launcher launches (no TTY)
exec 2>>/tmp/prism.log
echo "=== prism started: $(date) D=$D ===" >&2
echo "  user: $(id 2>&1)" >&2

# GLib gspawn opens /dev/null O_RDONLY for child stdin — ensure accessible.
# In the palm-launch sandbox /dev/null may be mode 000; fix it.
chmod 666 /dev/null 2>>/tmp/prism.log || true

# Open media hardware device nodes to all — required for Qualcomm OMX
# (libOmxCore.so allocates pmem via /dev/pmem_adsp; decoder uses /dev/msm_vidc_dec)
# These are group 'luna'; we need rw access regardless of our group.
chmod a+rw /dev/pmem_adsp /dev/msm_vidc_dec 2>/dev/null && \
    echo "  media devices opened OK" >&2 || \
    echo "  WARNING: could not chmod media devices (not root?)" >&2

export LD_LIBRARY_PATH="$D/lib:$LD_LIBRARY_PATH"
export WPE_BACKEND="$D/lib/libWPEBackend-sdl.so.0"
export WEBKIT_WEBPROCESS_PATH="$D/libexec/WPEWebProcess"
export WEBKIT_NETWORKPROCESS_PATH="$D/libexec/WPENetworkProcess"
export WEBKIT_EXEC_PATH="$D/libexec"
# LD_PRELOAD shim (currently passive — just marks load order)
export LD_PRELOAD="$D/lib/libswap-hook.so"
# Fontconfig: use our config that also searches the device's system fonts
export FONTCONFIG_PATH="$D/etc/fonts"
export FONTCONFIG_FILE="$D/etc/fonts/fonts.conf"
# GIO modules path for TLS (glib-networking, when available)
export GIO_MODULE_DIR="$D/lib/gio/modules"
# Limit JS heap to reduce "too many cards" OOM kills on heavy sites.
# JSC_OPTIONS is read by every JSC process (browser + WPEWebProcess).
export JSC_OPTIONS="--maxHeapSize=64 --useDFGJIT=false"
# GnuTLS CA trust store — copy bundled certs to the path compiled into libgnutls
cp "$D/certs/ca-certificates.crt" /tmp/prism-ca.crt || true
# GStreamer plugin path — only our bundled plugins, no system 0.10 plugins
export GST_PLUGIN_PATH="$D/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH="$D/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$D/libexec/gst-plugin-scanner"
export GST_REGISTRY="/tmp/prism-gst-registry.bin"
# Disable registry scanning of system GStreamer 0.10 paths
export GST_PLUGIN_PATH_1_0="$D/lib/gstreamer-1.0"
# GStreamer debug — log to /tmp/prism-gst.log (remove or set to 0 for production)
export GST_DEBUG="3,palmvideodec:5,playbin:4,uridecodebin:4,decodebin:4"
export GST_DEBUG_FILE="/tmp/prism-gst.log"
export GST_DEBUG_NO_COLOR=1

# Pre-create shared framebuffer so WPEWebProcess (different uid) can open it O_RDWR.
# Without this, open() fails with EACCES if the file was previously created by root.
rm -f /tmp/prism-fb
touch /tmp/prism-fb && chmod 666 /tmp/prism-fb || true

# webOS launcher passes params as JSON (e.g. '{ }' or '{"target":"http://..."}').
# Extract a real URL if present; otherwise load the home page.
_url="file://$D/home.html"
case "${1:-}" in
    http://*|https://*|file://*) _url="$1" ;;
esac
exec "$D/prism-browser" "$_url"
WRAPPER
chmod 755 "$APPDIR/prism"

# ── 3b. Home page ─────────────────────────────────────────────────────────
cp "$SCRIPT_DIR/prism-browser/home.html"        "$APPDIR/"
cp "$SCRIPT_DIR/prism-browser/test-images.html" "$APPDIR/"

# ── 4. Executables ─────────────────────────────────────────────────────────
install -m 755 "$OUT/bin/prism-browser"                         "$APPDIR/"
install -m 755 "$OUT/libexec/wpe-webkit-1.0/WPEWebProcess"     "$APPDIR/libexec/"
install -m 755 "$OUT/libexec/wpe-webkit-1.0/WPENetworkProcess" "$APPDIR/libexec/"
$STRIP --strip-unneeded "$APPDIR/prism-browser"
$STRIP --strip-unneeded "$APPDIR/libexec/WPEWebProcess"
$STRIP --strip-unneeded "$APPDIR/libexec/WPENetworkProcess"

# ── 5. Shared libraries ────────────────────────────────────────────────────
SHIP_LIBS=(
    # WPE
    "libWPEWebKit-1.0.so.3.18.7"
    "libwpe-1.0.so.1.10.0"
    "libWPEBackend-sdl.so.0.1.0"
    # ICU (JS engine requires these)
    "libicudata.so.67.1"
    "libicuuc.so.67.1"
    "libicui18n.so.67.1"
    # GLib 2.58
    "libglib-2.0.so.0.5800.3"
    "libgobject-2.0.so.0.5800.3"
    "libgio-2.0.so.0.5800.3"
    "libgmodule-2.0.so.0.5800.3"
    "libgthread-2.0.so.0.5800.3"
    # Text rendering
    "libcairo.so.2.11600.0"
    "libpixman-1.so.0.42.2"
    "libharfbuzz.so.0.50301.0"
    "libharfbuzz-icu.so.0.50301.0"
    "libfreetype.so.6.20.1"
    "libfontconfig.so.1.13.0"
    # Networking
    "libsoup-2.4.so.1.11.2"
    "libxml2.so.2.10.4"
    "libpsl.so.5.3.4"
    # Crypto
    "libgcrypt.so.20.4.2"
    "libgpg-error.so.0.33.1"
    # Images
    "libjpeg.so.8.2.2"
    "libwebp.so.7.1.8"
    "libwebpdemux.so.2.0.14"
    "libsharpyuv.so.0.0.1"
    # OpenGL abstraction
    "libepoxy.so.0.0.0"
    # Misc
    "libsqlite3.so.0.8.6"
    "libffi.so.8.1.2"
    "libexpat.so.1.9.2"
    # C++ runtime (device has GCC 4.3, our code needs GCC 10 symbols)
    "libstdc++.so.6.0.28"
    # LD_PRELOAD shim for FBO redirect + pixel readback
    "libswap-hook.so"
    # GStreamer 1.18.6 core + base libraries
    "libgstreamer-1.0.so.0.1806.0"
    "libgstbase-1.0.so.0.1806.0"
    "libgstapp-1.0.so.0.1806.0"
    "libgstvideo-1.0.so.0.1806.0"
    "libgstaudio-1.0.so.0.1806.0"
    "libgstpbutils-1.0.so.0.1806.0"
    "libgsttag-1.0.so.0.1806.0"
    "libgstallocators-1.0.so.0.1806.0"
    "libgstnet-1.0.so.0.1806.0"
    "libgstfft-1.0.so.0.1806.0"
    "libgstcontroller-1.0.so.0.1806.0"
    "libgstrtp-1.0.so.0.1806.0"
    "libgstriff-1.0.so.0.1806.0"
    "libgstsdp-1.0.so.0.1806.0"
    # FFmpeg 6.1 — provides AAC/MP3/Vorbis/H.264 software decode via gst-libav
    "libavcodec.so.60.31.102"
    "libavformat.so.60.16.100"
    "libavutil.so.58.29.100"
    "libswresample.so.4.12.100"
    # TLS stack: GnuTLS + dependencies (for glib-networking GIO module)
    "libgmp.so.10.5.0"
    "libnettle.so.8.8"
    "libhogweed.so.6.8"
    "libtasn1.so.6.6.3"
    "libgnutls.so.30.38.0"
)

for sofile in "${SHIP_LIBS[@]}"; do
    src="$OUT/lib/$sofile"
    if [ ! -f "$src" ]; then
        echo "WARNING: missing $sofile — skipping"
        continue
    fi
    cp "$src" "$APPDIR/lib/"
    $STRIP --strip-unneeded "$APPDIR/lib/$sofile" 2>/dev/null || true

    # Create the SONAME symlink (e.g. libfoo.so.6 -> libfoo.so.6.0.28)
    soname=$(objdump -p "$src" 2>/dev/null | awk '/SONAME/ {print $2}')
    if [ -n "$soname" ] && [ "$soname" != "$sofile" ]; then
        ln -sf "$sofile" "$APPDIR/lib/$soname"
    fi
done

# Extra symlinks for libs whose SONAME is missing but other libs reference by major version
# (e.g. libicudata has no SONAME but libicuuc/libicui18n need libicudata.so.67)
for versioned in "$APPDIR/lib"/*.so.*.*; do
    [ -L "$versioned" ] && continue
    base=$(basename "$versioned")
    # Strip last version component: libfoo.so.67.1 -> libfoo.so.67
    major=$(echo "$base" | sed 's/\.[0-9]*$//')
    if [ "$major" != "$base" ] && [ ! -e "$APPDIR/lib/$major" ]; then
        ln -sf "$base" "$APPDIR/lib/$major"
        echo "Extra symlink: $major -> $base"
    fi
done

# ── 6. GIO modules (TLS backend: libgiognutls.so) ─────────────────────────
GIO_MOD_SRC="$OUT/lib/gio/modules"
if [ -d "$GIO_MOD_SRC" ]; then
    for mod in "$GIO_MOD_SRC"/*.so; do
        [ -f "$mod" ] || continue
        cp "$mod" "$APPDIR/lib/gio/modules/"
        $STRIP --strip-unneeded "$APPDIR/lib/gio/modules/$(basename "$mod")" 2>/dev/null || true
        echo "GIO module: $(basename "$mod")"
    done
else
    echo "NOTE: $GIO_MOD_SRC not found — TLS not available (build glib-networking first)"
fi

# ── 7. GStreamer plugins ───────────────────────────────────────────────────
GST_PLUG_SRC="$OUT/lib/gstreamer-1.0"
GST_PLUGINS=(
    # Core
    "libgstcoreelements.so"
    "libgsttypefindfunctions.so"
    "libgstapp.so"          # appsink/appsrc — required by WebKit media pipeline
    # Playback pipeline
    "libgstplayback.so"
    "libgstaudioparsers.so"
    "libgstaudioconvert.so"
    "libgstaudioresample.so"
    "libgstaudiorate.so"
    "libgstaudiofx.so"      # scaletempo (playback rate), equalizer, etc.
    "libgstvideoconvert.so"
    "libgstvideoscale.so"
    "libgstvideorate.so"
    "libgstvolume.so"
    "libgstautodetect.so"
    # Demuxers / containers
    "libgstisomp4.so"       # MP4/M4V/M4A
    "libgstmatroska.so"     # WebM/MKV
    "libgstsubparse.so"     # subtitles
    # Network source
    "libgstsoup.so"         # HTTP(S) streaming via libsoup
    # Qualcomm HW video decoder
    "gstpalmvideodec.so"
    # FFmpeg-backed software decoders: AAC, MP3, Vorbis, H.264 fallback, etc.
    "libgstlibav.so"
)
for plug in "${GST_PLUGINS[@]}"; do
    src="$GST_PLUG_SRC/$plug"
    if [ ! -f "$src" ]; then
        echo "WARNING: missing GStreamer plugin $plug — skipping"
        continue
    fi
    cp "$src" "$APPDIR/lib/gstreamer-1.0/"
    $STRIP --strip-unneeded "$APPDIR/lib/gstreamer-1.0/$plug" 2>/dev/null || true
    echo "GStreamer plugin: $plug"
done

# gst-plugin-scanner binary (needed for registry generation at first launch)
GST_SCANNER="$OUT/libexec/gstreamer-1.0/gst-plugin-scanner"
if [ -f "$GST_SCANNER" ]; then
    install -m 755 "$GST_SCANNER" "$APPDIR/libexec/gst-plugin-scanner"
    $STRIP --strip-unneeded "$APPDIR/libexec/gst-plugin-scanner" 2>/dev/null || true
    echo "gst-plugin-scanner installed"
else
    echo "WARNING: gst-plugin-scanner not found at $GST_SCANNER"
fi

# ── 7b. Placeholder icon (replace with real 64x64 PNG later) ──────────────
if [ -f "$SCRIPT_DIR/icon.png" ]; then
    cp "$SCRIPT_DIR/icon.png" "$APPDIR/"
else
    # 1×1 transparent PNG
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' \
        > "$APPDIR/icon.png"
fi

# ── 8. Run palm-package ────────────────────────────────────────────────────
DATA_SIZE=$(du -sh "$APPDIR" | cut -f1)
echo "App directory: $DATA_SIZE"
echo "Running palm-package..."

palm-package -o "$OUTDIR" "$APPDIR"

# palm-package produces: <id>_<version>_all.ipk
IPK="$OUTDIR/${APP_ID}_1.0.0_all.ipk"
IPK_SIZE=$(du -sh "$IPK" | cut -f1)

echo ""
echo "Done: $IPK"
echo "  Uncompressed: $DATA_SIZE"
echo "  IPK:          $IPK_SIZE"
echo ""
echo "Install on device (TouchPad connected via USB):"
echo "  palm-install '$IPK'"
