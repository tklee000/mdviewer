# MdViewer for Linux

Ubuntu 22.04 LTS x86_64 is the baseline build and test environment. The native
shell uses GTK 3 and embeds CEF through X11. Wayland and Windows 11 WSLg run the
child browser through XWayland (`--ozone-platform=x11`). WSLg also uses software
compositing to avoid CEF GPU-process failures on incompatible Mesa/D3D12 stacks.

## Windows 11 WSLg quick start

Check WSL, WSLg, and the installed distributions from PowerShell:

```powershell
wsl --version
wsl --list --verbose
```

Install Ubuntu 22.04 only when it is not already listed:

```powershell
wsl --install --distribution Ubuntu-22.04
```

Inside Ubuntu, verify that WSLg supplied both display sockets:

```bash
grep PRETTY_NAME /etc/os-release
printf 'DISPLAY=%s\nWAYLAND_DISPLAY=%s\n' "$DISPLAY" "$WAYLAND_DISPLAY"
test -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
```

Keep the build tree in WSL's ext4 filesystem for better compile performance.
For this workspace, the Windows source can be copied without generated output:

```bash
mkdir -p "$HOME/mdviewer"
rsync -a \
  --exclude=.git/ --exclude=out/ --exclude=output/ \
  --exclude=tmp/ --exclude=x64/ --exclude=.vs/ \
  /mnt/d/ai_works/mdviewer/ "$HOME/mdviewer/"
cd "$HOME/mdviewer"
```

## Prerequisites (Ubuntu 22.04)

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  libgtk-3-dev libx11-dev libnss3 libxss1 libasound2 libgbm1 \
  libdrm2 libxcomposite1 libxdamage1 libxrandr2 \
  libatk-bridge2.0-0 libatspi2.0-0 libpango-1.0-0 libcups2 \
  libdbus-1-3 libxkbcommon0 libwayland-client0 \
  ca-certificates curl bzip2 rsync git dbus-x11 x11-utils \
  fonts-noto-cjk fonts-noto-color-emoji
```

Download and verify the Linux build matching the Windows CEF version:

```bash
mkdir -p "$HOME/.cache/mdviewer/cef"
cd "$HOME/.cache/mdviewer/cef"
curl -fL --retry 3 \
  -o 'cef_binary_151.3.17+gf059e67+chromium-151.0.7922.138_linux64_minimal.tar.bz2' \
  'https://cef-builds.spotifycdn.com/cef_binary_151.3.17%2Bgf059e67%2Bchromium-151.0.7922.138_linux64_minimal.tar.bz2'
echo '89915dd727a107d2c0f8e7b3e591715649cf5b75  cef_binary_151.3.17+gf059e67+chromium-151.0.7922.138_linux64_minimal.tar.bz2' \
  | sha1sum --check
tar -xjf cef_binary_151.3.17+gf059e67+chromium-151.0.7922.138_linux64_minimal.tar.bz2
export CEF_ROOT="$HOME/.cache/mdviewer/cef/cef_binary_151.3.17+gf059e67+chromium-151.0.7922.138_linux64_minimal"
```

Configure, build, and install for the current WSL user:

```bash
cd "$HOME/mdviewer"
cmake -S linux -B out/linux -G Ninja \
  -DCEF_ROOT="$CEF_ROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux --parallel
cmake --install out/linux --prefix "$HOME/.local"
```

Launch a WSLg window. `dbus-run-session` supplies a desktop session bus when a
plain `wsl` terminal does not already have one:

```bash
dbus-run-session -- "$HOME/.local/bin/mdviewer"
dbus-run-session -- "$HOME/.local/bin/mdviewer" "$HOME/mdviewer/README.md"
```

For automated GUI validation, launch with a temporary debugging port and run
the smoke test from a Windows terminal in the source workspace:

```bash
dbus-run-session -- "$HOME/.local/bin/mdviewer" \
  "$HOME/mdviewer/README.md" --remote-debugging-port=9223
```

```powershell
$env:MDVIEWER_EXPECT_TEXT = 'MdViewer'
node tests/linux-wslg-smoke.mjs
Remove-Item Env:MDVIEWER_EXPECT_TEXT
```

The test checks the CEF page, native bridge, Markdown startup-file loading, and
the actual `1180x760` WSLg viewport, then writes `out/linux-wslg-smoke.png`.

The Linux native shell supports Markdown and MDZ open/edit/save paths. The GTK
open dialog exposes Markdown and MDZip filters; Save As exposes both formats
plus UTF-8, UTF-8 BOM, UTF-16 LE, and UTF-16 BE choices for plain Markdown.
MDZ Markdown entries remain UTF-8 as required by the package format. Windows-
specific native export and direct-print services remain separate platform
integrations.

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
