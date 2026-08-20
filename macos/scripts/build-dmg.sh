#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MAC_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd -- "$MAC_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/out/macos-release}"
ICONSET="$BUILD_DIR/MdViewer.iconset"
ICON_FILE="$BUILD_DIR/MdViewer.icns"
ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-$(uname -m)}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"

case "$BUILD_DIR" in
  ""|/|"$HOME"|"$PROJECT_ROOT")
    echo "Refusing unsafe BUILD_DIR: $BUILD_DIR" >&2
    exit 4
    ;;
esac

if [[ -z "${CEF_ROOT:-}" ]]; then
  echo "CEF_ROOT must point to a macOS CEF distribution matching $ARCHITECTURES." >&2
  exit 2
fi

mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" "$PROJECT_ROOT/assets/MdViewer.png" \
    --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z "$double" "$double" "$PROJECT_ROOT/assets/MdViewer.png" \
    --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$ICON_FILE"

cmake -S "$MAC_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCEF_ROOT="$CEF_ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$ARCHITECTURES" \
  -DMDVIEWER_ICON_FILE="$ICON_FILE"
cmake --build "$BUILD_DIR" --parallel

APP="$BUILD_DIR/MdViewer.app"
if [[ ! -d "$APP" ]]; then
  echo "MdViewer.app was not produced at $APP" >&2
  exit 3
fi

while IFS= read -r -d '' helper; do
  codesign --force --deep --options runtime \
    --entitlements "$MAC_DIR/resources/entitlements.plist" \
    --sign "$SIGN_IDENTITY" "$helper"
done < <(find "$APP/Contents/Frameworks" -maxdepth 1 -name 'MdViewer Helper*.app' -print0)
codesign --force --deep --options runtime \
  --entitlements "$MAC_DIR/resources/entitlements.plist" \
  --sign "$SIGN_IDENTITY" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

STAGE="$BUILD_DIR/dmg-stage"
rm -rf -- "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/MdViewer.app"
ln -s /Applications "$STAGE/Applications"

OUTPUT="${OUTPUT:-$PROJECT_ROOT/out/MdViewer-0.2.0-$ARCHITECTURES.dmg}"
mkdir -p "$(dirname -- "$OUTPUT")"
rm -f -- "$OUTPUT"
hdiutil create -volname "MdViewer" -srcfolder "$STAGE" \
  -ov -format UDZO "$OUTPUT"
echo "Created $OUTPUT"
