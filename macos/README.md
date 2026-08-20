# MdViewer for macOS

The macOS target uses a Cocoa `NSWindow` with CEF embedded as a child view. It
loads the same editor UI and native bridge as the Windows and Linux targets.
The app bundle declares ownership of `.md` and `.markdown`, so Finder sends
document-open requests to the running app.

## Prerequisites

- macOS 12 or newer by default, subject to the selected CEF build's minimum,
  and current Xcode command-line tools
- CMake 3.21+ and Ninja
- A macOS CEF binary distribution for the selected architecture

Build directly:

```bash
export CEF_ROOT="$HOME/cef_binary_macos"
cmake -S macos -B out/macos -G Ninja \
  -DCEF_ROOT="$CEF_ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build out/macos --parallel
```

Create a DMG with the orange MdViewer icon and the CEF framework/helpers
embedded in `MdViewer.app`:

```bash
chmod +x macos/scripts/build-dmg.sh
CEF_ROOT="$HOME/cef_binary_macos" macos/scripts/build-dmg.sh
```

`SIGN_IDENTITY` defaults to ad-hoc signing (`-`). For distribution outside the
development machine, set it to a Developer ID Application identity and add the
normal notarization/stapling steps after `build-dmg.sh` completes. The CEF
architecture must match `CMAKE_OSX_ARCHITECTURES`; building a universal app
requires a universal CEF distribution.
