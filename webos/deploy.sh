#!/usr/bin/env bash
# Deploy Prism Browser to HP TouchPad via novacom
# Usage: ./deploy.sh [URL]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$SCRIPT_DIR/out"
DEVICE_ROOT="/media/internal/prism"
URL="${1:-https://start.duckduckgo.com}"

echo "=== Prism Browser Deploy ==="
echo "Bundling libraries..."

BUNDLE="$SCRIPT_DIR/build/prism-bundle"
rm -rf "$BUNDLE" && mkdir -p "$BUNDLE/lib" "$BUNDLE/libexec" "$BUNDLE/bin"

# Browser executable
cp "$OUT/bin/prism-browser" "$BUNDLE/bin/"

# WPE + backend
cp -P "$OUT/lib/libwpe-1.0.so"* "$BUNDLE/lib/"
cp -P "$OUT/lib/libWPEWebKit-1.0.so"* "$BUNDLE/lib/"
cp -P "$OUT/lib/libWPEBackend-sdl.so"* "$BUNDLE/lib/"

# WPE sub-processes
mkdir -p "$BUNDLE/libexec"
cp "$OUT/libexec/wpe-webkit-1.0/WPEWebProcess" "$BUNDLE/libexec/"
cp "$OUT/libexec/wpe-webkit-1.0/WPENetworkProcess" "$BUNDLE/libexec/"

# Runtime dependencies we built (not on device)
# Copy all versioned .so files from our prefix, excluding static libs
find "$OUT/lib" -maxdepth 1 \( -name "*.so*" \) \
    ! -name "*.a" ! -name "*.la" \
    ! -name "libSDL*" ! -name "libEGL*" ! -name "libGLES*" \
    ! -name "libOpenVG*" ! -name "libIMGegl*" ! -name "libsrv*" \
    ! -name "libicutest*" ! -name "libicutu*" ! -name "libicuio*" \
    -exec cp -P {} "$BUNDLE/lib/" \; 2>/dev/null || true

# Launcher script
cat > "$BUNDLE/bin/prism" << LAUNCHER
#!/bin/sh
PRISM_DIR="\$(dirname "\$(readlink -f "\$0")")/.."
export LD_LIBRARY_PATH="\$PRISM_DIR/lib:\$LD_LIBRARY_PATH"
export WPE_BACKEND="\$PRISM_DIR/lib/libWPEBackend-sdl.so"
export WEBKIT_WEBPROCESS_PATH="\$PRISM_DIR/libexec/WPEWebProcess"
export WEBKIT_NETWORKPROCESS_PATH="\$PRISM_DIR/libexec/WPENetworkProcess"
export WEBKIT_INSPECTOR_SERVER=127.0.0.1:9222
exec "\$PRISM_DIR/bin/prism-browser" "\$@"
LAUNCHER
chmod +x "$BUNDLE/bin/prism"

echo "Bundle contents:"
du -sh "$BUNDLE"
find "$BUNDLE" -type f | sort

echo ""
echo "Uploading to device at $DEVICE_ROOT ..."
novacom run file:///bin/mkdir -- -p "$DEVICE_ROOT" 2>/dev/null || true
tar -czf /tmp/prism-bundle.tar.gz -C "$BUNDLE" .
novacom put file://"$DEVICE_ROOT/prism-bundle.tar.gz" < /tmp/prism-bundle.tar.gz
novacom run file:///bin/sh -- -c "cd $DEVICE_ROOT && tar -xzf prism-bundle.tar.gz && rm prism-bundle.tar.gz"
novacom run file:///bin/sh -- -c "chmod +x $DEVICE_ROOT/bin/prism $DEVICE_ROOT/bin/prism-browser $DEVICE_ROOT/libexec/WPEWebProcess $DEVICE_ROOT/libexec/WPENetworkProcess"
echo ""
echo "Done! To run on device:"
echo "  novacom run file:///bin/sh -- -c '$DEVICE_ROOT/bin/prism $URL'"
