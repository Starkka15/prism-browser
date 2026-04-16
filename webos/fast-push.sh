#!/usr/bin/env bash
# fast-push.sh — push individual changed files to the device without full IPK reinstall.
#
# Usage:
#   ./fast-push.sh omx          — rebuild + push gstpalmvideodec.so
#   ./fast-push.sh webkit       — push libWPEWebKit (already built; does NOT rebuild)
#   ./fast-push.sh all          — omx + webkit
#   ./fast-push.sh gst-launch   — push gst-launch-1.0 + gst-inspect-1.0 to /tmp/gst/
#
# After pushing, restart the app on the device to pick up changes.
# For GStreamer tools, run via novacom:
#   novacom run file:///tmp/gst/gst-launch-1.0 -- <pipeline>
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$SCRIPT_DIR/out"
GCC10="$HOME/webos-touchpad-modernize/toolchain/gcc10/bin"
STRIP="$GCC10/arm-none-linux-gnueabi-strip"
APPDIR="/media/cryptofs/apps/usr/palm/applications/com.prism.browser"

# ── novacom push helper ────────────────────────────────────────────────────
push() {
    local src="$1"
    local dst="$2"
    echo "  → $dst"
    novacom put "file://$dst" < "$src"
}

strip_push() {
    local src="$1"
    local dst="$2"
    local tmp
    tmp=$(mktemp /tmp/fast-push-XXXXXX)
    cp "$src" "$tmp"
    "$STRIP" --strip-unneeded "$tmp" 2>/dev/null || true
    echo "  → $dst ($(du -sh "$tmp" | cut -f1))"
    # cryptofs does not allow overwriting existing files — must remove old first.
    # Strategy: push stripped binary to /tmp on device, then run a helper script
    # that does rm + cp from /tmp into the final destination.
    local devtmp="/tmp/fast-push-$(basename "$dst")"
    local helper="/tmp/fast-push-helper.sh"
    printf '#!/bin/sh\nrm -f "%s"\ncp "%s" "%s"\nrm -f "%s"\n' \
        "$dst" "$devtmp" "$dst" "$devtmp" > /tmp/fast-push-helper-local.sh
    novacom put "file://$devtmp" < "$tmp"
    novacom put "file://$helper" < /tmp/fast-push-helper-local.sh
    novacom run file://bin/sh -- "$helper"
    rm -f "$tmp" /tmp/fast-push-helper-local.sh
}

# ── Check device is reachable ──────────────────────────────────────────────
if ! novacom run file://bin/true -- 2>/dev/null; then
    echo "ERROR: device not reachable via novacom"
    exit 1
fi

do_omx() {
    echo "=== Building gst-palm-video-decoder ==="
    export PATH="$GCC10:$PATH"
    PKG_CONFIG_PATH="$OUT/lib/pkgconfig" PKG_CONFIG_LIBDIR="$OUT/lib/pkgconfig" \
        ninja -C "$SCRIPT_DIR/build/gst-palm-video-decoder" -j4
    ninja -C "$SCRIPT_DIR/build/gst-palm-video-decoder" install

    echo "=== Pushing gstpalmvideodec.so ==="
    strip_push "$OUT/lib/gstreamer-1.0/gstpalmvideodec.so" \
               "$APPDIR/lib/gstreamer-1.0/gstpalmvideodec.so"

    # Invalidate registry so GStreamer rescans
    novacom run file://bin/rm -- -f /tmp/prism-gst-registry.bin 2>/dev/null || true
    echo "=== OMX plugin pushed, registry cleared ==="
}

do_webkit() {
    echo "=== Pushing libWPEWebKit ==="
    local sofile="libWPEWebKit-1.0.so.3.18.7"
    strip_push "$OUT/lib/$sofile" "$APPDIR/lib/$sofile"
    # Symlinks already exist on device from IPK install; only the .so content changes
    echo "=== WebKit pushed ==="
}

do_gst_tools() {
    echo "=== Pushing GStreamer tools to /tmp/gst/ ==="
    novacom run file://bin/mkdir -- -p /tmp/gst
    strip_push "$SCRIPT_DIR/build/gstreamer/tools/gst-launch-1.0"  /tmp/gst/gst-launch-1.0
    strip_push "$SCRIPT_DIR/build/gstreamer/tools/gst-inspect-1.0" /tmp/gst/gst-inspect-1.0
    novacom run file://bin/chmod -- 755 /tmp/gst/gst-launch-1.0 /tmp/gst/gst-inspect-1.0

    echo ""
    echo "=== GStreamer tools available on device ==="
    echo "Example — test video decode from URL:"
    echo "  novacom run file:///tmp/gst/gst-launch-1.0 -- \\"
    echo "    -e souphttpsrc location=http://example.com/test.mp4 \\"
    echo "    ! qtdemux name=d \\"
    echo "    d.video_0 ! h264parse ! palmvideodec ! fakesink"
    echo ""
    echo "Inspect plugins:"
    echo "  novacom run file:///tmp/gst/gst-inspect-1.0 -- palmvideodec"
    echo "  novacom run file:///tmp/gst/gst-inspect-1.0 -- avdec_aac"
}

case "${1:-}" in
    omx)     do_omx ;;
    webkit)  do_webkit ;;
    all)     do_omx; do_webkit ;;
    gst-launch|gst) do_gst_tools ;;
    "")
        echo "Usage: $0 {omx|webkit|all|gst-launch}"
        exit 1
        ;;
    *)
        echo "Unknown target: $1"
        echo "Usage: $0 {omx|webkit|all|gst-launch}"
        exit 1
        ;;
esac
