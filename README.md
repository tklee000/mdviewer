# MdViewer

MdViewer is a CEF-based Markdown editor with two editable views. Windows is the
primary release target. The GTK3/X11 Linux shell is build- and GUI-smoke-tested
on Ubuntu 22.04 under Windows 11 WSLg; the macOS port is source-ready for
continued work on a native build host:

- **Source editing** edits the Markdown text directly.
- **Preview editing** edits the rendered document and serializes supported
  blocks back to Markdown.

The native shell is C++20 and the editor UI runs in CEF. Windows uses Win32,
Linux uses GTK3/X11, and macOS uses Cocoa. All three load the same editor UI.

## Current milestone

- Visual Studio 2019 16.11 or Visual Studio 2022, x64, C++20 mode
- CMake-generated Visual Studio solution
- CEF 151 minimal runtime, downloaded and checksum verified by `build.ps1`
- Open, edit, save, Save As, and command-line file opening for Markdown and MDZip
- Exact PDF export with in-app A4/Letter print preview, orientation, margins,
  backgrounds, optional page numbers, and atomic Save As
- Exact in-app print preview with printer selection, copy count, and all-pages
  or custom ranges such as `1-3, 5`, followed by direct printing without a
  second system dialog
- Editable DOCX export with Word headings, lists, tables, hyperlinks, code,
  images, A4/Letter page setup, metadata, Korean fonts, and atomic Save As
- Editable HWPX export with A4/Letter page setup, portrait/landscape layout,
  margins, document metadata, Korean fonts, tables, lists, code, and images
- System-browser Google Drive Picker opening with direct save-back and conflict detection
- A unified ten-item recent-document menu for local and Google Drive files
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
- Safe relative image loading from the Markdown document directory or directly from an MDZ archive
- External file-change detection
- Runtime UI language switching across 12 languages
- Japanese app chrome uses MS UI Gothic while document editors and previews
  retain their configured document fonts
- MiniGitGUI-style dark/light themes with runtime switching and persistence
- CEF UI translations backed by embedded JSON catalogs

The current preview serializer intentionally treats Markdown as the canonical
document format. YAML front matter is displayed as a protected block. Complex
extensions such as footnotes, math, Mermaid, and arbitrary
HTML still require the round-trip work described in `플랜.md`.

## Export

`File > Export…` (`Ctrl+Shift+E`) opens one export workflow. Choose PDF, DOCX,
or HWPX from the format selector, then review the format-specific settings and
preview before saving. The last selected format is remembered for the next
export.

## Printing

`File > Print…` (`Ctrl+P`) opens one in-app dialog containing the printer,
number of copies, page range, A4/Letter paper, orientation, margin, background,
and optional page-number settings. Choose all pages or enter one-based ranges
separated by commas, for example `1-3, 5, 8-10`. The preview is regenerated
with only those pages, so the document shown in the dialog is the document
sent to the selected Windows printer. Clicking **Print** submits the job
directly; a second system print dialog is not opened.

Click **Advanced settings…** beside the selected printer to open that printer
driver's document-properties sheet. Device-specific choices such as duplex
binding, color mode, paper source, output quality, and finishing are retained
for the current print dialog and passed to the direct print job. The in-app
paper, orientation, and copy controls remain authoritative so the job matches
the visible preview.

The direct path uses the Windows PDF renderer, WIC image decoder, and GDI print
spooler that are part of Windows. It adds no external print library or runtime
dependency.

## DOCX export

`File > Export…` (`Ctrl+Shift+E`), followed by the DOCX format choice, converts
the rendered Markdown into an editable Microsoft Word document. The dialog controls A4 or Letter paper,
orientation, 0/10/20 mm margins, title, author, Malgun Gothic or Batang body
font, and image embedding. Headings, numbered and bulleted lists, task items,
tables, hyperlinks, quotations, code blocks, and images use native WordprocessingML
structures rather than flattened screenshots.

The product implementation does not add a DOCX library or runtime dependency.
MdViewer writes the OOXML/OPC parts directly with C++20 and packages them using
its existing bundled CRC32/raw-DEFLATE implementation. Microsoft Word is only
used by the optional compatibility smoke test, not by the export feature.

## HWPX export

`File > Export…` (`Ctrl+Shift+E`), followed by the HWPX format choice, converts
the rendered Markdown document into an editable OWPML package. The export dialog provides a content preview and
controls for A4 or Letter paper, orientation, margins, title, author, body
font, and image embedding. The output is written atomically as `.hwpx` and
uses the standard `application/hwp+zip` package layout; no Hancom Office
installation is required to export it.

The content preview checks structure and styling, while final pagination is
performed by Hancom Office and can vary with the installed fonts and version.
Format implementation attribution is included under `licenses/owpml/`.

## MDZip documents

Saving a document with the `.mdz` extension switches it to MDZip document
mode. MdViewer writes a spec-versioned `manifest.json`, stores the canonical
Markdown in `index.md`, and keeps subsequently inserted PNG, JPEG, GIF, WebP,
or BMP images under `images/` inside the same archive. Preview images are
served directly from the in-memory archive without extracting files to disk.

MDZ entries use ZIP method 8 (raw RFC 1951 DEFLATE) through the bundled,
user-owned `minicell/libzip` v0.7 source. Its 7-Zip/LZMA2 portion is excluded.
Opening an existing `.mdz` accepts both STORE and DEFLATE entries, preserves
unknown safe entries, and enforces entry-count, expanded-size, traversal,
encryption, and CRC safety checks. Markdown content inside MDZ is always saved
as UTF-8; its selected LF/CRLF line ending is preserved.

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

- `linux/README.md`: Ubuntu 22.04, Windows 11 WSLg, and AppImage packaging
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
optional `.md`/`.markdown`/`.mdz` association task for packaged releases. Windows may
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

## Google Drive setup

Google Drive support uses the current desktop/mobile Picker flow in the system
browser, a desktop OAuth client, PKCE, a loopback redirect, and the narrow
`drive.file` scope. Refresh tokens
are stored in Windows Credential Manager; access tokens stay in process memory,
and neither token is written to the recent-document file.

Enable the Google Drive API and Google Picker API in one Google Cloud project,
configure its OAuth consent screen, and create a **Desktop app** OAuth client.
Put the client values in this local, untracked file:

```text
%LOCALAPPDATA%\MdViewer\google-drive.ini
```

```ini
[google]
clientId=DESKTOP_OAUTH_CLIENT_ID.apps.googleusercontent.com
clientSecret=DESKTOP_OAUTH_CLIENT_SECRET
```

You can import Google's downloaded desktop OAuth JSON without printing the
secret value:

```powershell
.\scripts\configure-google-drive.ps1 -CredentialsPath .\client_secret_....json
```

`File > Save to Google Drive As…` creates a new Markdown file in My Drive or
in a folder selected through the system-browser Google Picker. After creation,
the document becomes a Drive document and ordinary Save updates that same file.

The optional environment variables `MDVIEWER_GOOGLE_CLIENT_ID`,
and `MDVIEWER_GOOGLE_CLIENT_SECRET` override the file. OAuth projects left in
Google Cloud's Testing publishing status may require reauthorization after
their test refresh token expires.

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

## Regression tests

The standard randomized editor-history regression uses 20 operation types,
3 rounds, and 3 deterministic seeds. Each run exhausts undo, exhausts redo,
and exhausts undo again, then compares the source, dirty state, and preview DOM
with the initial document.

```powershell
node .\tests\ui-smoke.mjs
node .\tests\native-open-smoke.mjs
node .\tests\native-mdz-smoke.mjs
node .\tests\native-pdf-smoke.mjs
node .\tests\native-print-smoke.mjs
node .\tests\native-docx-smoke.mjs
node .\tests\native-hwpx-smoke.mjs
.\out\build\Release\MdViewerCoreTests.exe
```
