#!/usr/bin/env bash
# Deploy Prism Browser to HP TouchPad via novacom
set -e

OUT="$(cd "$(dirname "$0")/../out" && pwd)"
DEVICE="${WEBOS_DEVICE:-touchpad}"  # set WEBOS_DEVICE env var or pass as arg
INSTALL_PATH="/media/internal/prism"

echo "Deploying to $DEVICE..."

# Create install dir on device
novacom -d "$DEVICE" run -- mkdir -p "$INSTALL_PATH"

# Copy browser binary + WebKit libs
novacom -d "$DEVICE" put file://"$OUT/bin/prism" "$INSTALL_PATH/prism"
novacom -d "$DEVICE" put file://"$OUT/lib/libWebKit2.so" "$INSTALL_PATH/libWebKit2.so"
novacom -d "$DEVICE" put file://"$OUT/lib/libJavaScriptCore.so" "$INSTALL_PATH/libJavaScriptCore.so"
novacom -d "$DEVICE" put file://"$OUT/lib/libWTF.so" "$INSTALL_PATH/libWTF.so"

echo "Done. Run with:"
echo "  novacom -d $DEVICE run -- $INSTALL_PATH/prism"
