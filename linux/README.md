# MdViewer for Linux

Ubuntu 22.04 LTS x86_64 is the baseline build and test environment. The native
shell uses GTK 3 and embeds CEF through X11. Wayland sessions run through
XWayland (`--ozone-platform=x11`) for predictable CEF child-window behavior.

## Prerequisites (Ubuntu 22.04)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config \
  libgtk-3-dev libx11-dev libnss3 libxss1 libasound2 libgbm1
```

Download a Linux x86_64 CEF binary distribution built for a compatible glibc,
then build:

```bash
export CEF_ROOT="$HOME/cef_binary_linux64"
cmake -S linux -B out/linux -G Ninja \
  -DCEF_ROOT="$CEF_ROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux --parallel
cmake --install out/linux --prefix out/MdViewer.AppDir/usr
```

To create an AppImage, install or download `linuxdeploy` and `appimagetool`,
make the scripts executable once, and run:

```bash
chmod +x linux/scripts/build-appimage.sh linux/packaging/AppRun
LINUXDEPLOY=/path/to/linuxdeploy \
APPIMAGETOOL=/path/to/appimagetool linux/scripts/build-appimage.sh
```

Build the release AppImage on Ubuntu 22.04 (or an older compatible container),
because glibc compatibility flows from the build host to newer distributions.
CEF is run with `no_sandbox` until the AppImage supplies a correctly owned
setuid `chrome-sandbox` or a verified user-namespace sandbox strategy.
