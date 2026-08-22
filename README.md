# MdViewer

MdViewer is a CEF-based Markdown editor with two editable views. The current
validated release is Windows, with source-level Linux and macOS ports ready
for continued development on their native build hosts:

- **Source editing** edits the Markdown text directly.
- **Preview editing** edits the rendered document and serializes supported
  blocks back to Markdown.

The native shell is C++20 and the editor UI runs in CEF. Windows uses Win32,
Linux uses GTK3/X11, and macOS uses Cocoa. All three load the same editor UI.

## Current milestone

- Visual Studio 2019 16.11 or Visual Studio 2022, x64, C++20 mode
- CMake-generated Visual Studio solution
- CEF 151 minimal runtime, downloaded and checksum verified by `build.ps1`
- Open, edit, save, Save As, and command-line file opening
- Single-instance forwarding when another Markdown file is double-clicked
- UTF-8, UTF-8 BOM, CRLF/LF preservation and atomic replacement on save
- Custom title bar with File/Edit/View/Help menus and window controls
- Orange `M` application icon embedded in the executable and installer
- Source/preview editing switch from both the title bar menu and toolbar
- Editable headings, paragraphs, emphasis, links, images, lists, task lists,
  blockquotes, fenced code blocks, horizontal rules, and tables
- Professional formatting toolbar with undo/redo, strikethrough, inline/block
  code, list variants, image embedding, table grid insertion, contextual table
  row/column editing, indentation, and clear-formatting commands
- Command-level undo/redo for toolbar actions in both source and preview modes
- Safe relative image loading from the Markdown document directory
- External file-change detection
- Runtime UI language switching across 12 languages
- MiniGitGUI-style dark/light themes with runtime switching and persistence
- CEF UI translations backed by embedded JSON catalogs

The current preview serializer intentionally treats Markdown as the canonical
document format. YAML front matter is displayed as a protected block. Complex
extensions such as footnotes, math, Mermaid, and arbitrary
HTML still require the round-trip work described in `플랜.md`.

## Build

Requirements:

- Windows 10 or Windows 11
- Visual Studio 2019 16.11+ with **Desktop development with C++**
- Windows 10 SDK

Run:

```powershell
.\build.ps1 -Configuration Debug
```

To reuse an already extracted CEF distribution:

```powershell
.\build.ps1 -Configuration Debug `
  -CefRoot "D:\path\to\cef_binary_151..._windows64_minimal"
```

The generated solution is `out\build\MdViewer.sln`, and the runnable output is
`x64\Debug\MdViewer.exe`.

Create a clean release directory with:

```powershell
.\build.ps1 -Configuration Release -Deploy
```

Platform-specific build and packaging instructions are available here:

- `linux/README.md`: Ubuntu 22.04 baseline and AppImage packaging
- `macos/README.md`: Cocoa app bundle, CEF helpers, signing, and DMG packaging

## File association

The application accepts a Markdown path as its first command-line argument.
At startup it registers itself as an available Markdown application and checks
the effective `.md` association. If no default Markdown application points to
MdViewer, the user is asked before the default is changed. Existing MdViewer
associations that reference an older executable path are repaired silently.
When Windows has a protected `UserChoice` for another application, MdViewer
opens Default Apps after consent because Windows does not permit applications
to overwrite that protected choice directly.

For a development build, register the current executable for the current user:

```powershell
.\scripts\register-file-association.ps1 `
  -ExecutablePath ".\x64\Release\MdViewer.exe"
```

The Inno Setup definition under `installer\windows\MdViewer.iss` provides an
optional `.md`/`.markdown` association task for packaged releases. Windows may
still ask the user to confirm the default application according to its Default
Apps policy.

## Localization

English source strings are the fallback catalog. The other catalogs live under
`web\locales\`. The same files are compiled as Win32 `RCDATA` for native
dialogs and startup errors, then served to the CEF UI through the embedded
application origin. The language and theme settings are stored in:

```text
%LOCALAPPDATA%\MdViewer\config.ini
```

Validate catalog key and placeholder parity with:

```powershell
.\scripts\validate-locales.ps1
```

## Project structure

```text
src/                    Native app, file handling, localization, CEF bridge
platform/common/        Portable document controller and CEF/JavaScript bridge
linux/                  Ubuntu 22.04 GTK3 shell and AppImage packaging
macos/                  Cocoa shell, CEF helper bundles, and DMG packaging
assets/                 Application icon source and generated Windows icon
web/                    Embedded editable UI and Markdown conversion
web/locales/            Shared JSON translation catalogs
installer/windows/      Windows installer definition
scripts/                Development file-association helpers
tests/                   Markdown round-trip fixtures
```
