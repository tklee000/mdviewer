#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LINUX_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd -- "$LINUX_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/out/linux-release}"
APPDIR="${APPDIR:-$PROJECT_ROOT/out/MdViewer.AppDir}"
APPIMAGETOOL="${APPIMAGETOOL:-appimagetool}"
LINUXDEPLOY="${LINUXDEPLOY:-linuxdeploy}"

case "$APPDIR" in
  ""|/|"$HOME"|"$PROJECT_ROOT")
    echo "Refusing unsafe APPDIR: $APPDIR" >&2
    exit 5
    ;;
esac

if [[ -z "${CEF_ROOT:-}" ]]; then
  echo "CEF_ROOT must point to a Linux 64-bit CEF binary distribution." >&2
  exit 2
fi
if ! command -v "$APPIMAGETOOL" >/dev/null 2>&1; then
  echo "appimagetool was not found. Set APPIMAGETOOL=/path/to/appimagetool." >&2
  exit 3
fi
if ! command -v "$LINUXDEPLOY" >/dev/null 2>&1; then
  echo "linuxdeploy was not found. Set LINUXDEPLOY=/path/to/linuxdeploy." >&2
  exit 4
fi

cmake -S "$LINUX_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCEF_ROOT="$CEF_ROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel
rm -rf -- "$APPDIR"
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"

"$LINUXDEPLOY" --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/mdviewer" \
  --desktop-file "$APPDIR/usr/share/applications/mdviewer.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/mdviewer.png"
install -Dm755 "$LINUX_DIR/packaging/AppRun" "$APPDIR/AppRun"
ln -sfn "usr/share/applications/mdviewer.desktop" "$APPDIR/mdviewer.desktop"
ln -sfn "usr/share/icons/hicolor/256x256/apps/mdviewer.png" "$APPDIR/mdviewer.png"

OUTPUT="${OUTPUT:-$PROJECT_ROOT/out/MdViewer-x86_64.AppImage}"
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"
echo "Created $OUTPUT"
