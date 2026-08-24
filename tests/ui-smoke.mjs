import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const workspace = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const webRoot = join(workspace, "web");
const edge = process.env.MDVIEWER_EDGE ||
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
const mimeTypes = new Map([
  [".html", "text/html; charset=utf-8"], [".js", "text/javascript; charset=utf-8"],
  [".css", "text/css; charset=utf-8"], [".json", "application/json; charset=utf-8"]
]);
const positiveIntegerEnvironment = (name, fallback) => {
  const value = Number(process.env[name]);
  return Number.isInteger(value) && value > 0 ? value : fallback;
};
const randomHistorySeedCount = positiveIntegerEnvironment("MDVIEWER_RANDOM_SEEDS", 3);
const randomHistoryRounds = positiveIntegerEnvironment("MDVIEWER_RANDOM_ROUNDS", 3);
const randomHistoryRedoCycles = process.env.MDVIEWER_RANDOM_REDO !== "0";

const server = createServer(async (request, response) => {
  try {
    const requestPath = decodeURIComponent(new URL(request.url, "http://localhost").pathname);
    const relative = requestPath === "/" ? "index.html" : requestPath.slice(1);
    const path = resolve(webRoot, relative);
    if (path !== webRoot && !path.startsWith(`${webRoot}${sep}`)) throw new Error("Invalid path");
    const bytes = await readFile(path);
    response.writeHead(200, { "content-type": mimeTypes.get(extname(path)) || "application/octet-stream" });
    response.end(bytes);
  } catch {
    if (!response.headersSent) response.writeHead(404);
    response.end("Not found");
  }
});

await new Promise((resolveListen) => server.listen(0, "127.0.0.1", resolveListen));
const port = server.address().port;
const debuggingPort = port + 1;
const profile = await mkdtemp(join(tmpdir(), "mdviewer-ui-test-"));
if (dirname(resolve(profile)) !== resolve(tmpdir()) ||
    !profile.slice(profile.lastIndexOf(sep) + 1).startsWith("mdviewer-ui-test-")) {
  throw new Error(`Unsafe temporary browser profile path: ${profile}`);
}
const browser = spawn(edge, [
  "--headless=new", "--disable-gpu", "--no-first-run", "--disable-extensions",
  `--remote-debugging-port=${debuggingPort}`, `--user-data-dir=${profile}`,
  `http://127.0.0.1:${port}/?lang=en-US`
], { stdio: "ignore", windowsHide: true });

const delay = (milliseconds) => new Promise((resolveDelay) => setTimeout(resolveDelay, milliseconds));
let socket;
let nextId = 1;
const pending = new Map();

async function connect() {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${debuggingPort}/json/list`)).json();
      const target = targets.find((entry) => entry.type === "page");
      if (target) {
        socket = new WebSocket(target.webSocketDebuggerUrl);
        await new Promise((resolveOpen, rejectOpen) => {
          socket.addEventListener("open", resolveOpen, { once: true });
          socket.addEventListener("error", rejectOpen, { once: true });
        });
        socket.addEventListener("message", (event) => {
          const message = JSON.parse(event.data);
          if (!message.id || !pending.has(message.id)) return;
          const { resolveMessage, rejectMessage } = pending.get(message.id);
          pending.delete(message.id);
          if (message.error) rejectMessage(new Error(message.error.message));
          else resolveMessage(message.result);
        });
        return;
      }
    } catch { /* Browser is still starting. */ }
    await delay(100);
  }
  throw new Error("Could not connect to the headless browser.");
}

function send(method, params = {}) {
  const id = nextId++;
  return new Promise((resolveMessage, rejectMessage) => {
    pending.set(id, { resolveMessage, rejectMessage });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

async function evaluate(expression) {
  const result = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (result.exceptionDetails) {
    throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  }
  return result.result.value;
}

const js = (value) => JSON.stringify(value);

async function setSource(text, start = 0, end = text.length) {
  await evaluate(`(() => {
    const editor = document.querySelector('#source-editor');
    if (document.querySelector('#source-pane').hidden) {
      document.querySelector('[data-status-mode="source"]').click();
    }
    editor.value = ${js(text)};
    editor.focus();
    editor.setSelectionRange(${start}, ${end});
    editor.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertText' }));
  })()`);
}

async function sourceValue() {
  return evaluate("document.querySelector('#source-editor').value");
}

async function setEditorMode(mode) {
  await evaluate(`(() => {
    if (document.querySelector('#mode-toggle-button').dataset.currentMode !== ${js(mode)}) {
      document.querySelector('[data-status-mode="${mode}"]').click();
    }
  })()`);
}

async function loadCleanDocument(text) {
  await setSource(text, 0, 0);
  await evaluate(`window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
    type: 'document.saved', document: { name: 'history-test.md', encoding: 'UTF-8', eol: 'LF' }
  } }))`);
}

async function click(selector) {
  await evaluate(`(() => {
    const element = document.querySelector(${js(selector)});
    element.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    element.click();
  })()`);
}

async function verifySourceCommand(before, selector, after, start = 0, end = before.length) {
  await setSource(before, start, end);
  await click(selector);
  assert.equal(await sourceValue(), after, selector);
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), before, `${selector} undo`);
  await click('[data-editor-command="redo"]');
  assert.equal(await sourceValue(), after, `${selector} redo`);
}

async function setPreviewSelection(markdown, selector = "#preview-editor p", collapse = false) {
  await setSource(markdown);
  await setEditorMode("preview");
  await evaluate(`(() => {
    const target = document.querySelector(${js(selector)});
    const range = document.createRange();
    range.selectNodeContents(target);
    if (${collapse}) range.collapse(false);
    const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
    target.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
  })()`);
}

async function verifyPreviewCommand(markdown, targetSelector, commandSelector, resultExpression,
                                    collapse = false) {
  await setPreviewSelection(markdown, targetSelector, collapse);
  const before = await evaluate("document.querySelector('#preview-editor').innerHTML");
  await click(commandSelector);
  const after = await evaluate("document.querySelector('#preview-editor').innerHTML");
  assert.equal(await evaluate(resultExpression), true, `preview ${commandSelector}: ${after}`);
  assert.notEqual(after, before, `preview ${commandSelector} changed`);
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), before,
    `preview ${commandSelector} undo`);
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), after,
    `preview ${commandSelector} redo`);
}

try {
  await connect();
  await send("Runtime.enable");
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (await evaluate("Boolean(document.querySelector('#table-size-grid button'))")) break;
    await delay(50);
  }

  const initialModeLayout = await evaluate(`({
    previewVisible: !document.querySelector('#preview-pane').hidden,
    sourceHidden: document.querySelector('#source-pane').hidden,
    currentMode: document.querySelector('#mode-toggle-button').dataset.currentMode,
    toolbarModeSwitchRemoved: !document.querySelector('.toolbar .mode-switch'),
    toggleIsFirstStatusItem: document.querySelector('.statusbar').firstElementChild?.id === 'mode-toggle-button'
  })`);
  assert.deepEqual(initialModeLayout, {
    previewVisible: true, sourceHidden: true, currentMode: "preview",
    toolbarModeSwitchRemoved: true, toggleIsFirstStatusItem: true
  }, "preview is the default and mode switching lives at bottom left");
  const japaneseUiFonts = await evaluate(`(async () => {
    await window.MdViewerI18n.setLocale('ja-JP');
    const font = (selector) => getComputedStyle(document.querySelector(selector)).fontFamily;
    const result = {
      locale: document.documentElement.lang,
      menu: font('.menu-trigger'),
      toolbar: font('.toolbar'),
      dialog: font('#pdf-export-dialog'),
      statusbar: font('.statusbar'),
      source: font('#source-editor'),
      preview: font('#preview-editor'),
      docxPreview: font('#docx-document-preview'),
      hwpxPreview: font('#hwpx-document-preview')
    };
    await window.MdViewerI18n.setLocale('en-US');
    result.englishMenu = font('.menu-trigger');
    return result;
  })()`);
  assert.equal(japaneseUiFonts.locale, "ja-JP", "Japanese locale updates the document language");
  for (const area of ["menu", "toolbar", "dialog", "statusbar"]) {
    assert.match(japaneseUiFonts[area], /MS UI Gothic/i,
      `${area} uses MS UI Gothic for Japanese UI`);
  }
  for (const area of ["source", "preview", "docxPreview", "hwpxPreview"]) {
    assert.doesNotMatch(japaneseUiFonts[area], /MS UI Gothic/i,
      `${area} keeps its document font in Japanese UI`);
  }
  assert.doesNotMatch(japaneseUiFonts.englishMenu, /MS UI Gothic/i,
    "non-Japanese UI restores the normal application font");
  const exportDialogLayout = await evaluate(`(() => {
    const definitions = [
      ['pdf-export-dialog', '.pdf-export-settings'],
      ['docx-export-dialog', '.hwpx-export-settings'],
      ['hwpx-export-dialog', '.hwpx-export-settings']
    ];
    return definitions.map(([id, settingsSelector]) => {
      const dialog = document.getElementById(id);
      dialog.showModal();
      const rounded = (element) => {
        const rect = element.getBoundingClientRect();
        return { width: Math.round(rect.width), height: Math.round(rect.height) };
      };
      const result = {
        dialog: rounded(dialog),
        header: rounded(dialog.querySelector('.settings-dialog-header')),
        settings: rounded(dialog.querySelector(settingsSelector)),
        previewToolbar: rounded(dialog.querySelector('.export-preview-toolbar'))
      };
      dialog.close();
      return result;
    });
  })()`);
  for (const property of ['dialog', 'header', 'settings', 'previewToolbar']) {
    assert.deepEqual(exportDialogLayout.map(layout => layout[property]),
      Array(3).fill(exportDialogLayout[0][property]),
      `PDF, DOCX, and HWPX export ${property} dimensions stay aligned`);
  }
  assert.deepEqual(await evaluate(`[
    getComputedStyle(document.querySelector('#docx-document-preview')).borderRadius,
    getComputedStyle(document.querySelector('#hwpx-document-preview')).borderRadius
  ]`), ['0px', '0px'],
  "DOCX and HWPX preview pages use square corners");
  assert.deepEqual(await evaluate(`[
    getComputedStyle(document.querySelector('#pdf-preview-frame')).backgroundColor,
    getComputedStyle(document.querySelector('#docx-document-preview').parentElement).backgroundColor,
    getComputedStyle(document.querySelector('#hwpx-document-preview').parentElement).backgroundColor
  ]`), Array(3).fill('rgb(82, 86, 89)'),
  "PDF, DOCX, and HWPX preview surfaces use the same gray background");
  const darkThemeNeutrals = await evaluate(`(() => {
    const style = getComputedStyle(document.documentElement);
    return ['--bg', '--stage', '--surface', '--surface-soft', '--surface-raised',
      '--line', '--line-strong'].map(name => style.getPropertyValue(name).trim());
  })()`);
  assert.deepEqual(darkThemeNeutrals,
    ["#121212", "#181818", "#202020", "#282828", "#303030", "#3a3a3a", "#505050"],
    "dark theme surfaces use neutral gray instead of blue-gray");
  assert.equal(await evaluate(`(() => {
    const preview = document.querySelector('#preview-editor');
    preview.focus();
    return getComputedStyle(preview).outlineStyle;
  })()`), "none", "preview editing does not show an accent focus outline");

  const explorerDrop = await evaluate(`(() => {
    const stage = document.querySelector('.editor-stage');
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    const transfer = new DataTransfer();
    transfer.items.add(new File(['# dropped'], 'dropped.md', { type: 'text/markdown' }));
    const enter = new DragEvent('dragenter', {
      bubbles: true, cancelable: true, dataTransfer: transfer
    });
    const over = new DragEvent('dragover', {
      bubbles: true, cancelable: true, dataTransfer: transfer
    });
    const drop = new DragEvent('drop', {
      bubbles: true, cancelable: true, dataTransfer: transfer
    });
    stage.dispatchEvent(enter);
    stage.dispatchEvent(over);
    const highlighted = stage.classList.contains('is-file-drag-over');
    stage.dispatchEvent(drop);
    window.mdViewerNative = originalBridge;
    return {
      enterPrevented: enter.defaultPrevented,
      overPrevented: over.defaultPrevented,
      dropPrevented: drop.defaultPrevented,
      highlighted,
      highlightCleared: !stage.classList.contains('is-file-drag-over'),
      messages
    };
  })()`);
  assert.deepEqual(explorerDrop, {
    enterPrevented: true, overPrevented: true, dropPrevented: true,
    highlighted: true, highlightCleared: true,
    messages: [{ type: "files.dropped" }]
  }, "Explorer file drop is accepted by the editor stage and forwarded once");

  const recentDocuments = await evaluate(`(() => {
    const documents = Array.from({ length: 12 }, (_, index) => ({
      kind: index % 2 ? 'local' : 'googleDrive',
      location: index % 2 ? 'C:\\\\docs\\\\recent-' + index + '.md' : 'drive-' + index,
      name: 'recent-' + index + '.md',
      lastOpened: 1000 - index
    }));
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'recent.changed', documents }
    }));
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('#recent-documents-menu button').click();
    document.querySelector('[data-menu-command="file.openGoogleDrive"]').click();
    window.mdViewerNative = originalBridge;
    const buttons = [...document.querySelectorAll('#recent-documents-menu button')];
    return {
      count: buttons.length,
      driveMarks: document.querySelectorAll('#recent-documents-menu .google-drive-mark').length,
      firstTitle: buttons[0].title,
      messages
    };
  })()`);
  assert.deepEqual(recentDocuments, {
    count: 10,
    driveMarks: 5,
    firstTitle: "Google Drive: recent-0.md",
    messages: [
      { type: "recent.open", kind: "googleDrive", location: "drive-0" },
      { type: "command", name: "file.openGoogleDrive" }
    ]
  }, "recent menu limits entries, marks Drive files, and routes both commands");

  const driveSaveAs = await evaluate(`(() => {
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-menu-command="file.saveGoogleDriveAs"]').click();
    const dialog = document.querySelector('#google-drive-save-dialog');
    const opened = dialog.open;
    document.querySelector('#google-drive-file-name').value = 'new-cloud-note';
    document.querySelector('#google-drive-choose-folder').checked = false;
    document.querySelector('#google-drive-save-form').requestSubmit();
    window.mdViewerNative = originalBridge;
    return { opened, closed: !dialog.open, messages };
  })()`);
  assert.deepEqual(driveSaveAs, {
    opened: true,
    closed: true,
    messages: [{
      type: "command", name: "file.saveGoogleDriveAs",
      fileName: "new-cloud-note.md", chooseFolder: false
    }]
  }, "Drive Save As collects a Markdown name and routes the root upload command");

  assert.deepEqual(await evaluate(`(() => {
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-menu-command="file.saveGoogleDriveAs"]').click();
    const dialog = document.querySelector('#google-drive-save-dialog');
    const input = document.querySelector('#google-drive-file-name');
    input.value = 'wrong-extension.txt';
    document.querySelector('#google-drive-save-form').requestSubmit();
    const result = { open: dialog.open, invalid: !input.checkValidity(), messages };
    dialog.close('test');
    window.mdViewerNative = originalBridge;
    return result;
  })()`), { open: true, invalid: true, messages: [] },
  "Drive Save As rejects non-Markdown file names before native upload");

  assert.deepEqual(await evaluate(`(() => {
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-file-menu-root] > .menu-trigger').click();
    window.mdViewerNative = originalBridge;
    return messages;
  })()`), [{ type: "recent.refresh" }],
  "opening File refreshes recents shared by other MdViewer windows");
  assert.deepEqual(await evaluate(`[
    ...document.querySelectorAll('[data-menu-command^="file.export"]')
  ].map(item => item.dataset.menuCommand)`), ["file.export"],
  "File exposes one unified Export command");

  const pdfExport = await evaluate(`(async () => {
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    localStorage.setItem('mdviewer.exportFormat', 'pdf');
    document.querySelector('[data-menu-command="file.export"]').click();
    await new Promise(resolve => setTimeout(resolve, 100));
    const dialog = document.querySelector('#pdf-export-dialog');
    const opened = dialog.open;
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    const backdropStayedOpen = dialog.open;
    const format = dialog.querySelector('[data-export-format-select]').value;
    document.querySelector('#pdf-paper-select').value = 'letter';
    document.querySelector('#pdf-paper-select').dispatchEvent(new Event('change', { bubbles: true }));
    const landscape = document.querySelector('input[name="pdf-orientation"][value="landscape"]');
    landscape.checked = true;
    landscape.dispatchEvent(new Event('change', { bubbles: true }));
    document.querySelector('#pdf-margin-select').value = '10';
    document.querySelector('#pdf-margin-select').dispatchEvent(new Event('change', { bubbles: true }));
    document.querySelector('#pdf-page-numbers').checked = true;
    document.querySelector('#pdf-page-numbers').dispatchEvent(new Event('change', { bubbles: true }));
    document.querySelector('#pdf-print-background').checked = false;
    document.querySelector('#pdf-print-background').dispatchEvent(new Event('change', { bubbles: true }));
    await new Promise(resolve => setTimeout(resolve, 450));
    const previews = messages.filter(message => message.type === 'pdf.preview');
    const latest = previews.at(-1);
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'pdf.previewReady', requestId: latest.requestId,
      url: 'https://app.mdviewer/__pdf-preview?request=' + latest.requestId
    } }));
    const ready = !document.querySelector('#pdf-export-save').disabled;
    document.querySelector('#pdf-export-save').click();
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'pdf.saveCanceled' }
    }));
    document.querySelector('[data-pdf-export-cancel]').click();
    window.mdViewerNative = originalBridge;
    return { opened, backdropStayedOpen, format, ready, latest, messages, closed: !dialog.open };
  })()`);
  assert.equal(pdfExport.opened, true, "unified Export menu opens the PDF export dialog");
  assert.equal(pdfExport.backdropStayedOpen, true,
    "clicking outside the PDF export content does not close the dialog");
  assert.equal(pdfExport.format, "pdf", "the export dialog shows the active PDF format");
  assert.equal(pdfExport.ready, true, "native PDF preview enables exact-file save");
  assert.deepEqual({
    paper: pdfExport.latest.paper,
    orientation: pdfExport.latest.orientation,
    marginMm: pdfExport.latest.marginMm,
    pageNumbers: pdfExport.latest.pageNumbers,
    printBackground: pdfExport.latest.printBackground
  }, {
    paper: "letter", orientation: "landscape", marginMm: 10,
    pageNumbers: true, printBackground: false
  }, "PDF controls route validated print settings to native code");
  assert.deepEqual(pdfExport.messages.slice(-2).map(message => message.type),
    ["pdf.save", "pdf.previewClose"],
    "PDF save uses the visible preview and closing releases native preview data");
  assert.equal(pdfExport.closed, true, "PDF export dialog closes cleanly");

  const printPreview = await evaluate(`(async () => {
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-menu-command="file.print"]').click();
    await new Promise(resolve => setTimeout(resolve, 120));
    const dialog = document.querySelector('#pdf-export-dialog');
    const opened = dialog.open;
    const title = document.querySelector('#pdf-export-dialog-title').textContent;
    const pageRangeVisible = !document.querySelector('#pdf-page-range-settings').hidden;
    const printerSettingsVisible = !document.querySelector('#pdf-printer-settings').hidden;
    const printLabel = document.querySelector('#pdf-export-save').textContent;
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'printer.listed', printers: [
        { name: 'Test Office Printer', isDefault: true },
        { name: 'Test PDF Printer', isDefault: false }
      ]
    } }));
    const printer = document.querySelector('#pdf-printer-select');
    printer.value = 'Test Office Printer';
    printer.dispatchEvent(new Event('change', { bubbles: true }));
    const propertiesButton = document.querySelector('#pdf-printer-properties');
    const propertiesAvailable = !propertiesButton.disabled;
    propertiesButton.click();
    const propertiesRequest = messages.at(-1);
    const propertiesBusy = propertiesButton.disabled && printer.disabled &&
      document.querySelector('#pdf-export-save').disabled;
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'printer.propertiesApplied', printerName: 'Test Office Printer'
    } }));
    const propertiesApplied = !propertiesButton.disabled &&
      document.querySelector('#pdf-printer-properties-status').textContent ===
        'Advanced printer settings applied';
    const copies = document.querySelector('#pdf-print-copies');
    copies.value = '3';
    copies.dispatchEvent(new Event('input', { bubbles: true }));
    const previewsBeforeInvalid = messages.filter(message => message.type === 'pdf.preview').length;

    const custom = document.querySelector('input[name="pdf-print-pages"][value="custom"]');
    custom.checked = true;
    custom.dispatchEvent(new Event('change', { bubbles: true }));
    const range = document.querySelector('#pdf-page-range-input');
    range.value = '4-2';
    range.dispatchEvent(new Event('input', { bubbles: true }));
    await new Promise(resolve => setTimeout(resolve, 420));
    const invalid = range.getAttribute('aria-invalid') === 'true' &&
      document.querySelector('#pdf-page-range-error').hidden === false &&
      document.querySelector('#pdf-export-save').disabled;
    const invalidPreviewBlocked = messages.filter(
      message => message.type === 'pdf.preview').length === previewsBeforeInvalid;

    range.value = '2-4, 7';
    range.dispatchEvent(new Event('input', { bubbles: true }));
    await new Promise(resolve => setTimeout(resolve, 460));
    const previews = messages.filter(message => message.type === 'pdf.preview');
    const latest = previews.at(-1);
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'pdf.previewReady', requestId: latest.requestId,
      url: 'https://app.mdviewer/__pdf-preview?request=' + latest.requestId
    } }));
    document.querySelector('#pdf-preview-frame').dispatchEvent(new Event('load'));
    const ready = !document.querySelector('#pdf-export-save').disabled;
    document.querySelector('#pdf-export-save').click();
    const printRequest = messages.at(-1);
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'pdf.printStarted' }
    }));
    const busy = document.querySelector('#pdf-export-save').disabled &&
      document.querySelector('[data-pdf-export-cancel]').disabled;
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: {
        type: 'pdf.printed', printerName: 'Test Office Printer',
        copies: 3, pageCount: 4
      }
    }));
    window.mdViewerNative = originalBridge;
    return {
      opened, title, pageRangeVisible, printerSettingsVisible, printLabel, invalid,
      propertiesAvailable, propertiesRequest, propertiesBusy, propertiesApplied,
      invalidPreviewBlocked, latest, ready, printRequest, busy,
      closed: !dialog.open
    };
  })()`);
  assert.equal(printPreview.opened, true, "Print opens a modal preview from File");
  assert.equal(printPreview.title, "Print", "Print reuses the exact PDF preview dialog in print mode");
  assert.equal(printPreview.pageRangeVisible, true, "Print exposes page-range controls");
  assert.equal(printPreview.printerSettingsVisible, true,
    "the first print dialog exposes printer and copy controls");
  assert.equal(printPreview.propertiesAvailable, true,
    "advanced printer settings are available for the selected printer");
  assert.deepEqual(printPreview.propertiesRequest, {
    type: "printer.properties", printerName: "Test Office Printer"
  }, "advanced settings requests the selected printer's driver properties");
  assert.equal(printPreview.propertiesBusy, true,
    "printing controls are locked while printer properties are open");
  assert.equal(printPreview.propertiesApplied, true,
    "applied driver properties are reported in the first print dialog");
  assert.equal(printPreview.printLabel, "Print…", "Print mode has a printer action");
  assert.equal(printPreview.invalid, true, "descending page ranges are rejected inline");
  assert.equal(printPreview.invalidPreviewBlocked, true,
    "invalid page ranges do not request a stale native preview");
  assert.equal(printPreview.latest.pageRanges, "2-4,7",
    "print page ranges are normalized before native preview generation");
  assert.equal(printPreview.ready, true,
    "the printer action waits until the ranged PDF frame is loaded");
  assert.deepEqual(printPreview.printRequest, {
    type: "pdf.print", requestId: printPreview.latest.requestId,
    printerName: "Test Office Printer", copies: 3
  }, "Print sends the first dialog's printer and copies directly to native code");
  assert.equal(printPreview.busy, true,
    "the first dialog remains locked while the direct print job is spooled");
  assert.equal(printPreview.closed, true, "Print preview dialog closes cleanly");
  assert.equal(await evaluate(`(() => {
    document.dispatchEvent(new KeyboardEvent('keydown', {
      key: 'p', ctrlKey: true, bubbles: true, cancelable: true
    }));
    const dialog = document.querySelector('#pdf-export-dialog');
    const opened = dialog.open &&
      document.querySelector('#pdf-export-dialog-title').textContent === 'Print';
    document.querySelector('[data-pdf-export-cancel]').click();
    return opened && !dialog.open;
  })()`), true, "Ctrl+P opens and closes the print preview");

  const docxExport = await evaluate(`(async () => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'document.opened', mode: 'preview', document: {
        origin: 'local', path: 'C:\\notes\\워드.md', name: '워드.md',
        text: '# 제목\\n\\n본문 **굵게**와 [링크](https://example.com/docs?a=1&b=2)\\n\\n1. 첫째\\n2. 둘째\\n\\n| 열 A | 열 B |\\n| --- | --- |\\n| 값 1 | 값 2 |\\n\\n> 인용문\\n\\n\`\`\`js\\nconst answer = 42;\\n\`\`\`',
        dirty: false, encoding: 'UTF-8', eol: 'LF'
      }
    } }));
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-menu-command="file.export"]').click();
    const pdfDialog = document.querySelector('#pdf-export-dialog');
    const formatSelect = pdfDialog.querySelector('[data-export-format-select]');
    formatSelect.value = 'docx';
    formatSelect.dispatchEvent(new Event('change', { bubbles: true }));
    const dialog = document.querySelector('#docx-export-dialog');
    const opened = dialog.open;
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    const backdropStayedOpen = dialog.open;
    const switchedFromPdf = !pdfDialog.open &&
      dialog.querySelector('[data-export-format-select]').value === 'docx';
    document.querySelector('#docx-paper-select').value = 'letter';
    const landscape = document.querySelector('input[name="docx-orientation"][value="landscape"]');
    landscape.checked = true;
    landscape.dispatchEvent(new Event('change', { bubbles: true }));
    document.querySelector('#docx-margin-select').value = '10';
    document.querySelector('#docx-font-select').value = 'sans';
    document.querySelector('#docx-title-input').value = '내보낸 Word 문서';
    document.querySelector('#docx-author-input').value = 'MdViewer 테스트';
    document.querySelector('#docx-export-save').click();
    for (let attempt = 0; attempt < 50 && !messages.some(message =>
      message.type === 'docx.export'); attempt += 1) {
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    const exported = messages.find(message => message.type === 'docx.export');
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'docx.saveCanceled' }
    }));
    document.querySelector('[data-docx-export-cancel]').click();
    window.mdViewerNative = originalBridge;
    return { opened, backdropStayedOpen, switchedFromPdf, exported, closed: !dialog.open };
  })()`);
  assert.equal(docxExport.opened, true, "DOCX opens from the unified export format selector");
  assert.equal(docxExport.backdropStayedOpen, true,
    "clicking outside the DOCX export content does not close the dialog");
  assert.equal(docxExport.switchedFromPdf, true,
    "the unified export dialog switches from PDF to DOCX");
  assert.deepEqual({
    paper: docxExport.exported.paper,
    orientation: docxExport.exported.orientation,
    marginMm: docxExport.exported.marginMm,
    font: docxExport.exported.font,
    title: docxExport.exported.title,
    author: docxExport.exported.author
  }, {
    paper: "letter", orientation: "landscape", marginMm: 10, font: "sans",
    title: "내보낸 Word 문서", author: "MdViewer 테스트"
  }, "DOCX controls route page and document metadata to native code");
  assert.match(docxExport.exported.documentXml,
    /<w:pgSz w:w="15840" w:h="12240" w:orient="landscape"\/>/,
    "DOCX document uses Letter landscape dimensions");
  assert.match(docxExport.exported.documentXml,
    /<w:pgMar w:top="567" w:right="567" w:bottom="567" w:left="567"/,
    "DOCX document uses 10 mm margins");
  assert.match(docxExport.exported.documentXml, /<w:pStyle w:val="Heading1"\/>/,
    "DOCX uses editable Word heading styles");
  assert.match(docxExport.exported.documentXml, /<w:numPr>/,
    "DOCX uses editable Word numbering");
  assert.match(docxExport.exported.documentXml, /<w:tbl>/,
    "DOCX preserves Markdown tables as Word tables");
  assert.match(docxExport.exported.documentXml, /<w:hyperlink r:id="rIdLink1"/,
    "DOCX preserves links as Word hyperlinks");
  assert.match(docxExport.exported.hyperlinks, /^link1\thttps:\/\/example\.com\//,
    "DOCX sends safe hyperlink relationships to native packaging");
  assert.match(docxExport.exported.lists, /^ordered\t1/m,
    "DOCX sends real numbering definitions to native packaging");
  assert.equal(docxExport.closed, true, "DOCX export dialog closes cleanly");

  const hwpxExport = await evaluate(`(async () => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'document.opened', mode: 'preview', document: {
        origin: 'local', path: 'C:\\\\notes\\\\한글.md', name: '한글.md',
        text: '# 제목\\n\\n본문 **굵게**\\n\\n- 목록\\n\\n| 열 A | 열 B |\\n| --- | --- |\\n| 값 1 | 값 2 |\\n\\n\`\`\`js\\nconst answer = 42;\\n\`\`\`',
        dirty: false, encoding: 'UTF-8', eol: 'LF'
      }
    } }));
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-menu-command="file.export"]').click();
    const docxDialog = document.querySelector('#docx-export-dialog');
    const formatSelect = docxDialog.querySelector('[data-export-format-select]');
    formatSelect.value = 'hwpx';
    formatSelect.dispatchEvent(new Event('change', { bubbles: true }));
    const dialog = document.querySelector('#hwpx-export-dialog');
    const opened = dialog.open;
    dialog.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    const backdropStayedOpen = dialog.open;
    const switchedFromDocx = !docxDialog.open &&
      dialog.querySelector('[data-export-format-select]').value === 'hwpx';
    document.querySelector('#hwpx-paper-select').value = 'letter';
    const landscape = document.querySelector('input[name="hwpx-orientation"][value="landscape"]');
    landscape.checked = true;
    landscape.dispatchEvent(new Event('change', { bubbles: true }));
    document.querySelector('#hwpx-margin-select').value = '10';
    document.querySelector('#hwpx-font-select').value = 'sans';
    document.querySelector('#hwpx-title-input').value = '내보낸 문서';
    document.querySelector('#hwpx-author-input').value = 'MdViewer 테스트';
    document.querySelector('#hwpx-export-save').click();
    for (let attempt = 0; attempt < 50 && !messages.some(message =>
      message.type === 'hwpx.export'); attempt += 1) {
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    const exported = messages.find(message => message.type === 'hwpx.export');
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'hwpx.saveCanceled' }
    }));
    document.querySelector('[data-hwpx-export-cancel]').click();
    window.mdViewerNative = originalBridge;
    return { opened, backdropStayedOpen, switchedFromDocx, exported, closed: !dialog.open };
  })()`);
  assert.equal(hwpxExport.opened, true, "HWPX opens from the unified export format selector");
  assert.equal(hwpxExport.backdropStayedOpen, true,
    "clicking outside the HWPX export content does not close the dialog");
  assert.equal(hwpxExport.switchedFromDocx, true,
    "the unified export dialog switches from DOCX to HWPX");
  assert.deepEqual({
    paper: hwpxExport.exported.paper,
    orientation: hwpxExport.exported.orientation,
    marginMm: hwpxExport.exported.marginMm,
    font: hwpxExport.exported.font,
    title: hwpxExport.exported.title,
    author: hwpxExport.exported.author
  }, {
    paper: "letter", orientation: "landscape", marginMm: 10, font: "sans",
    title: "내보낸 문서", author: "MdViewer 테스트"
  }, "HWPX controls route page and document metadata to native code");
  assert.match(hwpxExport.exported.sectionXml, /<hp:secPr /,
    "HWPX section contains editable page properties");
  assert.match(hwpxExport.exported.sectionXml,
    /<hp:pagePr landscape="WIDELY" width="79200" height="61200"[^>]*><hp:margin[^>]*left="2835"[^>]*right="2835"[^>]*top="2835"[^>]*bottom="2835"/,
    "HWPX section uses Letter landscape dimensions and 10 mm margins");
  assert.match(hwpxExport.exported.sectionXml, /<hp:tbl /,
    "HWPX section preserves Markdown tables");
  assert.match(hwpxExport.exported.sectionXml, /제목/,
    "HWPX section preserves Unicode text");
  assert.equal(hwpxExport.closed, true, "HWPX export dialog closes cleanly");
  assert.equal(await evaluate(`(() => {
    document.dispatchEvent(new KeyboardEvent('keydown', {
      key: 'e', ctrlKey: true, shiftKey: true, bubbles: true, cancelable: true
    }));
    const dialog = document.querySelector('#hwpx-export-dialog');
    const opened = dialog.open &&
      dialog.querySelector('[data-export-format-select]').value === 'hwpx';
    document.querySelector('[data-hwpx-export-cancel]').click();
    return opened && !dialog.open;
  })()`), true, "Ctrl+Shift+E reopens the unified exporter in its last format");

  const driveDocumentChrome = await evaluate(`(async () => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'document.opened', mode: 'preview', document: {
        origin: 'googleDrive', path: '', name: 'cloud-note.md', text: '# Cloud',
        dirty: false, encoding: 'UTF-8', eol: 'LF'
      }
    } }));
    await new Promise(requestAnimationFrame);
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'googleDrive.busy', busy: true }
    }));
    const result = {
      title: document.querySelector('#document-name').textContent,
      status: document.querySelector('#save-status').textContent
    };
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', {
      detail: { type: 'googleDrive.busy', busy: false }
    }));
    return result;
  })()`);
  assert.deepEqual(driveDocumentChrome,
    { title: "cloud-note.md", status: "Working with Google Drive…" },
    "Drive documents keep their cloud name and expose operation progress");

  const driveSaveToast = await evaluate(`(async () => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'document.saved', document: {
        origin: 'googleDrive', path: '', name: 'cloud-note.md',
        encoding: 'UTF-8', eol: 'LF'
      }
    } }));
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const toast = document.querySelector('#toast-region .toast-notification');
    const shown = {
      tone: toast?.classList.contains('success') || false,
      role: toast?.getAttribute('role') || '',
      message: toast?.querySelector('.toast-message')?.textContent || ''
    };
    await new Promise(resolve => setTimeout(resolve, 3100));
    return {
      shown,
      dismissed: !document.querySelector('#toast-region .toast-notification')
    };
  })()`);
  assert.deepEqual(driveSaveToast, {
    shown: {
      tone: true,
      role: "status",
      message: "Saved to Google Drive: cloud-note.md"
    },
    dismissed: true
  }, "Drive save confirmation appears as a non-blocking auto-dismiss notification");

  assert.deepEqual(await evaluate(`(async () => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'native.toast', title: 'File save error',
      message: 'The document could not be saved.', tone: 'error'
    } }));
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const toast = document.querySelector('#toast-region .toast-notification');
    const result = {
      tone: toast?.classList.contains('error') || false,
      role: toast?.getAttribute('role') || '',
      icon: toast?.querySelector('.toast-icon')?.textContent || '',
      title: toast?.querySelector('.toast-title')?.textContent || '',
      message: toast?.querySelector('.toast-message')?.textContent || ''
    };
    document.querySelector('#toast-region').replaceChildren();
    return result;
  })()`), {
    tone: true,
    role: "alert",
    icon: "×",
    title: "File save error",
    message: "The document could not be saved."
  }, "native information, warning, and error messages use the common notification surface");

  assert.deepEqual(await evaluate(`(() => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'document.savedSnapshot',
      document: {
        origin: 'googleDrive', path: '', name: 'cloud-copy.md', dirty: true,
        encoding: 'UTF-8', eol: 'LF'
      },
      savedText: '# Earlier cloud revision', savedEol: 'LF'
    } }));
    return {
      title: document.querySelector('#document-name').textContent,
      dirty: !document.querySelector('#dirty-indicator').hidden
    };
  })()`), { title: "cloud-copy.md", dirty: true },
  "Drive Save As can relocate a document while preserving newer dirty edits");

  await click("#mode-toggle-button");
  assert.equal(await evaluate("!document.querySelector('#status-mode-menu').hidden"), true,
    "bottom-left mode button opens the view menu");
  await click('[data-status-mode="source"]');
  assert.equal(await evaluate("!document.querySelector('#source-pane').hidden"), true,
    "bottom-left mode button switches to source");
  await click("#mode-toggle-button");
  await click('[data-status-mode="preview"]');
  assert.equal(await evaluate("!document.querySelector('#preview-pane').hidden"), true,
    "bottom-left mode button switches back to preview");

  await send("Emulation.setDeviceMetricsOverride", {
    width: 1200, height: 800, deviceScaleFactor: 1, mobile: false
  });
  await setEditorMode("preview");
  const fittedPreviewLayout = await evaluate(`(() => {
    const pane = document.querySelector('#preview-pane');
    const editor = document.querySelector('#preview-editor');
    const paneStyle = getComputedStyle(pane);
    const paneBounds = pane.getBoundingClientRect();
    const editorBounds = editor.getBoundingClientRect();
    return {
      availableWidth: pane.clientWidth - parseFloat(paneStyle.paddingLeft) -
        parseFloat(paneStyle.paddingRight),
      editorWidth: editor.getBoundingClientRect().width,
      paddingTop: parseFloat(paneStyle.paddingTop),
      paddingRight: parseFloat(paneStyle.paddingRight),
      paddingBottom: parseFloat(paneStyle.paddingBottom),
      paddingLeft: parseFloat(paneStyle.paddingLeft),
      actualTopGap: editorBounds.top - paneBounds.top,
      actualRightGap: paneBounds.right - editorBounds.right,
      actualBottomGap: paneBounds.bottom - editorBounds.bottom,
      actualLeftGap: editorBounds.left - paneBounds.left
    };
  })()`);
  assert.ok(Math.abs(fittedPreviewLayout.editorWidth - fittedPreviewLayout.availableWidth) <= 1,
    `preview fills the available window width: ${JSON.stringify(fittedPreviewLayout)}`);
  assert.deepEqual({
    top: fittedPreviewLayout.paddingTop,
    right: fittedPreviewLayout.paddingRight,
    bottom: fittedPreviewLayout.paddingBottom,
    left: fittedPreviewLayout.paddingLeft
  }, { top: 20, right: 20, bottom: 16, left: 20 },
  "preview outer gray margin uses the reduced bottom gap");
  assert.deepEqual({
    top: fittedPreviewLayout.actualTopGap,
    right: fittedPreviewLayout.actualRightGap,
    bottom: fittedPreviewLayout.actualBottomGap,
    left: fittedPreviewLayout.actualLeftGap
  }, { top: 20, right: 20, bottom: 16, left: 20 },
  "preview card has the reduced actual bottom gap");
  await setSource("alpha", 0, 5);
  await setEditorMode("split");
  const splitLayout = await evaluate(`(() => {
    const source = document.querySelector('#source-pane').getBoundingClientRect();
    const preview = document.querySelector('#preview-pane').getBoundingClientRect();
    const divider = document.querySelector('#split-divider').getBoundingClientRect();
    return {
      mode: document.querySelector('#mode-toggle-button').dataset.currentMode,
      sourceVisible: !document.querySelector('#source-pane').hidden,
      previewVisible: !document.querySelector('#preview-pane').hidden,
      dividerVisible: !document.querySelector('#split-divider').hidden,
      sourceWidth: source.width,
      previewWidth: preview.width,
      dividerWidth: divider.width
    };
  })()`);
  assert.equal(splitLayout.mode, "split", "bottom-left menu selects split mode");
  assert.equal(splitLayout.sourceVisible && splitLayout.previewVisible && splitLayout.dividerVisible, true,
    `split panes visible: ${JSON.stringify(splitLayout)}`);
  assert.ok(Math.abs(splitLayout.sourceWidth - splitLayout.previewWidth) <= 8,
    `split starts at 50:50: ${JSON.stringify(splitLayout)}`);
  assert.equal(splitLayout.dividerWidth, 6, "split divider width");

  await click('[data-format="bold"]');
  assert.equal(await sourceValue(), "**alpha**", "split source toolbar formatting");
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor strong'))"), true,
    "split source changes update preview immediately");
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), "alpha", "split source formatting undo");
  assert.equal(await evaluate("document.querySelector('#preview-editor').textContent.trim()"), "alpha",
    "split source undo updates preview");

  await evaluate(`(() => {
    const editor = document.querySelector('#preview-editor');
    editor.focus();
    const text = editor.querySelector('p').firstChild;
    const range = document.createRange(); range.selectNodeContents(text);
    const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
    text.parentElement.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
  })()`);
  await click('[data-format="italic"]');
  assert.equal(await sourceValue(), "*alpha*", "split preview changes update source immediately");
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor em, #preview-editor i'))"), true,
    "split preview toolbar formatting");
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), "alpha", "split preview formatting undo updates source");
  assert.equal(await evaluate("document.querySelector('#preview-editor').textContent.trim()"), "alpha",
    "split preview formatting undo");

  const splitRatio = await evaluate(`(() => {
    const divider = document.querySelector('#split-divider');
    divider.focus();
    divider.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true }));
    const adjusted = getComputedStyle(document.querySelector('.editor-stage'))
      .getPropertyValue('--split-source-width').trim();
    divider.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    const reset = getComputedStyle(document.querySelector('.editor-stage'))
      .getPropertyValue('--split-source-width').trim();
    return { adjusted, reset, stored: localStorage.getItem('mdviewer.splitRatio') };
  })()`);
  assert.deepEqual(splitRatio, { adjusted: "48%", reset: "50%", stored: "50" },
    "split divider keyboard adjustment and reset");

  const longSplitMarkdown = Array.from({ length: 80 }, (_, index) =>
    `## Section ${index + 1}\n\nParagraph ${index + 1}`).join("\n\n");
  await setSource(longSplitMarkdown, 0, 0);
  await evaluate(`(() => {
    const source = document.querySelector('#source-editor');
    source.scrollTop = (source.scrollHeight - source.clientHeight) * 0.65;
    source.dispatchEvent(new Event('scroll'));
  })()`);
  await delay(60);
  assert.ok(await evaluate("document.querySelector('#preview-pane').scrollTop") > 0,
    "split source scrolling synchronizes preview");
  await evaluate(`(() => {
    const preview = document.querySelector('#preview-pane');
    document.querySelector('#preview-editor').focus();
    preview.scrollTop = (preview.scrollHeight - preview.clientHeight) * 0.8;
    preview.dispatchEvent(new Event('scroll'));
  })()`);
  await delay(60);
  assert.ok(await evaluate("document.querySelector('#source-editor').scrollTop") > 0,
    "split preview scrolling synchronizes source");

  await send("Emulation.setDeviceMetricsOverride", {
    width: 800, height: 760, deviceScaleFactor: 1, mobile: false
  });
  await delay(50);
  assert.equal(await evaluate(`(() => {
    return document.querySelector('#mode-toggle-button').dataset.currentMode === 'preview' &&
      document.querySelector('#source-pane').hidden && document.querySelector('#split-divider').hidden;
  })()`), true, "narrow window falls back from split to the active single editor");
  await send("Emulation.setDeviceMetricsOverride", {
    width: 1200, height: 800, deviceScaleFactor: 1, mobile: false
  });

  await verifySourceCommand("alpha", '[data-format="bold"]', "**alpha**");
  await verifySourceCommand("alpha", '[data-format="italic"]', "*alpha*");
  await verifySourceCommand("alpha", '[data-format="strike"]', "~~alpha~~");
  await verifySourceCommand("alpha", '[data-format="inlineCode"]', "`alpha`");
  await verifySourceCommand("alpha", '[data-format="codeBlock"]', "```\nalpha\n```");
  await evaluate("window.prompt = () => 'https://example.com'");
  await verifySourceCommand("alpha", '[data-format="link"]', "[alpha](https://example.com)");
  await verifySourceCommand("**bold** and *italic*", '[data-format="clear"]', "bold and italic");
  await verifySourceCommand("one\ntwo", '[data-block-format="bulletList"]', "- one\n- two");
  await verifySourceCommand("one\ntwo", '[data-block-format="orderedList"]', "1. one\n2. two");
  await verifySourceCommand("one\ntwo", '[data-block-format="taskList"]', "- [ ] one\n- [ ] two");
  await verifySourceCommand("one\ntwo", '[data-block-format="blockquote"]', "> one\n> two");
  await verifySourceCommand("- one", '[data-block-format="indent"]', "  - one");
  await verifySourceCommand("  - one", '[data-block-format="outdent"]', "- one");
  await verifySourceCommand("before", '[data-block-format="horizontalRule"]', "before\n\n---", 6, 6);
  assert.equal(await evaluate(`(() => {
    const icon = document.querySelector('[data-format="image"] svg.toolbar-icon');
    return Boolean(icon?.querySelector('rect') && icon?.querySelector('circle') &&
      icon?.querySelector('path'));
  })()`), true, "image toolbar button uses a recognizable picture icon");

  await setSource("");
  await click('[data-format="image"]');
  await evaluate(`(async () => {
    document.querySelector('#image-alt-input').value = 'cat';
    document.querySelector('#image-source-input').value = 'images/cat.png';
    document.querySelector('#image-dialog-form').requestSubmit();
    await new Promise(requestAnimationFrame);
  })()`);
  assert.equal(await sourceValue(), "![cat](images/cat.png)", "image insert");
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), "", "image undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await sourceValue(), "![cat](images/cat.png)", "image redo");

  const mdzImageRequest = await evaluate(`(async () => {
    window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
      type: 'document.opened', mode: 'source', document: {
        origin: 'local', format: 'mdz', path: 'C:\\\\docs\\\\bundle.mdz',
        name: 'bundle.mdz', text: '', dirty: false, encoding: 'UTF-8', eol: 'LF'
      }
    } }));
    const messages = [];
    const originalBridge = window.mdViewerNative;
    window.mdViewerNative = { postMessage: value => messages.push(JSON.parse(value)) };
    document.querySelector('[data-format="image"]').click();
    document.querySelector('#image-alt-input').value = 'packed';
    document.querySelector('#image-source-input').value =
      'data:image/png;base64,iVBORw0KGgo=';
    document.querySelector('#image-dialog-form').requestSubmit();
    await new Promise(requestAnimationFrame);
    window.mdViewerNative = originalBridge;
    return {
      messages,
      source: document.querySelector('#source-editor').value,
      encoding: document.querySelector('#encoding-status').textContent
    };
  })()`);
  assert.equal(mdzImageRequest.messages.length, 1, "MDZ image sends one native package request");
  assert.equal(mdzImageRequest.messages[0].type, "image.embed", "MDZ image uses native package bridge");
  assert.equal(mdzImageRequest.messages[0].fileName, "image", "manual MDZ data image receives a safe name");
  assert.equal(mdzImageRequest.source, "", "MDZ image link waits for native package success");
  assert.equal(mdzImageRequest.encoding, "MDZ · UTF-8", "status bar identifies MDZ mode");
  await evaluate(`window.dispatchEvent(new CustomEvent('mdviewerhostmessage', { detail: {
    type: 'image.embedded', path: 'images/image.png', alt: 'packed'
  } }))`);
  assert.equal(await sourceValue(), "![packed](images/image.png)",
    "MDZ package success inserts the archive-relative image link");
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), "", "MDZ image link undo restores the initial source");
  await click('[data-editor-command="redo"]');
  assert.equal(await sourceValue(), "![packed](images/image.png)",
    "MDZ image link redo restores the packaged image reference");

  await setSource("");
  await click('#table-size-grid [data-table-columns="3"][data-table-rows="2"]');
  const insertedTable = await sourceValue();
  assert.match(insertedTable, /^\| Column 1 \| Column 2 \| Column 3 \|/);
  assert.equal(insertedTable.split("\n").length, 4);
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), "", "table insert undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await sourceValue(), insertedTable, "table insert redo");

  const bodyPosition = insertedTable.lastIndexOf("|  |  |  |");
  await evaluate(`(() => {
    const editor = document.querySelector('#source-editor');
    editor.focus(); editor.setSelectionRange(${bodyPosition}, ${bodyPosition});
    editor.dispatchEvent(new Event('select', { bubbles: true }));
  })()`);
  await click('[data-table-command="rowAfter"]');
  const tableWithRow = await sourceValue();
  assert.equal(tableWithRow.split("\n").length, 5, "table row add");
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), insertedTable, "table row undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await sourceValue(), tableWithRow, "table row redo");

  await setSource("");
  await click('#table-size-grid [data-table-columns="2"][data-table-rows="2"]');
  await setEditorMode("preview");
  const emptySourceTableLayout = await evaluate(`(() => {
    const table = document.querySelector('#preview-editor table');
    const headerHeight = table.tHead.rows[0].getBoundingClientRect().height;
    const bodyHeights = [...table.tBodies[0].rows]
      .map((row) => row.getBoundingClientRect().height);
    return { headerHeight, bodyHeights, bodyRows: table.tBodies[0].rows.length };
  })()`);
  assert.equal(emptySourceTableLayout.bodyRows, 2, "source 2×2 table keeps both empty rows");
  assert.equal(emptySourceTableLayout.bodyHeights.every(
    (height) => Math.abs(height - emptySourceTableLayout.headerHeight) <= 1), true,
  `source 2×2 empty row heights: ${JSON.stringify(emptySourceTableLayout)}`);
  const sourceInsertedTwoByTwo = await sourceValue();
  await click('[data-editor-command="undo"]');
  assert.equal(await sourceValue(), "", "preview undo cancels source table insertion");
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor table'))"), false,
    "preview rerenders after undoing source table insertion");
  await click('[data-editor-command="redo"]');
  assert.equal(await sourceValue(), sourceInsertedTwoByTwo,
    "preview redo restores source table insertion");
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor table'))"), true,
    "preview rerenders after redoing source table insertion");

  await setSource("alpha");
  await setEditorMode("preview");
  await evaluate(`(() => {
    const text = document.querySelector('#preview-editor p').firstChild;
    const range = document.createRange(); range.selectNodeContents(text);
    const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
  })()`);
  await click('[data-format="bold"]');
  const previewBoldState = await evaluate(`({
    html: document.querySelector('#preview-editor').innerHTML,
    selection: getSelection().toString(),
    saved: document.querySelector('#source-editor').value,
    mode: document.querySelector('#preview-pane').hidden ? 'source' : 'preview'
  })`);
  assert.equal(/<(?:strong|b)>/.test(previewBoldState.html), true,
    `preview bold: ${JSON.stringify(previewBoldState)}`);
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor strong, #preview-editor b'))"), false, "preview bold undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor strong, #preview-editor b'))"), true, "preview bold redo");

  await verifyPreviewCommand("alpha", "#preview-editor p", '[data-format="italic"]',
    "Boolean(document.querySelector('#preview-editor em, #preview-editor i'))");
  await verifyPreviewCommand("alpha", "#preview-editor p", '[data-format="strike"]',
    "Boolean(document.querySelector('#preview-editor del, #preview-editor s, #preview-editor strike'))");
  await verifyPreviewCommand("alpha", "#preview-editor p", '[data-format="inlineCode"]',
    "Boolean(document.querySelector('#preview-editor p code'))");
  await verifyPreviewCommand("alpha", "#preview-editor p", '[data-format="codeBlock"]',
    "Boolean(document.querySelector('#preview-editor pre'))");
  await verifyPreviewCommand("alpha", "#preview-editor p", '[data-format="link"]',
    "Boolean(document.querySelector('#preview-editor a[href]'))");
  await verifyPreviewCommand("**alpha**", "#preview-editor strong", '[data-format="clear"]',
    "!document.querySelector('#preview-editor strong, #preview-editor b')");
  await verifyPreviewCommand("one", "#preview-editor p", '[data-block-format="bulletList"]',
    "Boolean(document.querySelector('#preview-editor ul > li'))");
  await verifyPreviewCommand("one", "#preview-editor p", '[data-block-format="orderedList"]',
    "Boolean(document.querySelector('#preview-editor ol > li'))");
  await verifyPreviewCommand("one", "#preview-editor p", '[data-block-format="taskList"]',
    "Boolean(document.querySelector('#preview-editor li[data-task=true] > input[type=checkbox]'))");
  await verifyPreviewCommand("one", "#preview-editor p", '[data-block-format="blockquote"]',
    "Boolean(document.querySelector('#preview-editor blockquote'))");
  await verifyPreviewCommand("one", "#preview-editor p", '[data-block-format="horizontalRule"]',
    "Boolean(document.querySelector('#preview-editor hr'))", true);
  await verifyPreviewCommand("- one\n- two", "#preview-editor ul > li:nth-child(2)",
    '[data-block-format="indent"]',
    "Boolean(document.querySelector('#preview-editor ul ul > li'))");
  await verifyPreviewCommand("- one\n  - two", "#preview-editor ul ul > li",
    '[data-block-format="outdent"]',
    "document.querySelectorAll('#preview-editor > ul > li').length === 2");

  await setPreviewSelection("- [ ] task", "#preview-editor input[type=checkbox]", true);
  const taskCheckbox = "#preview-editor input[type=checkbox]";
  await click(taskCheckbox);
  assert.equal(await evaluate(`document.querySelector(${js(taskCheckbox)}).checked`), true,
    "preview task checkbox");
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate(`document.querySelector(${js(taskCheckbox)}).checked`), false,
    "preview task checkbox undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate(`document.querySelector(${js(taskCheckbox)}).checked`), true,
    "preview task checkbox redo");

  await setPreviewSelection("alpha", "#preview-editor p", true);
  const previewBeforeImage = await evaluate("document.querySelector('#preview-editor').innerHTML");
  await click('[data-format="image"]');
  await evaluate(`(async () => {
    document.querySelector('#image-alt-input').value = 'cat';
    document.querySelector('#image-source-input').value = 'images/cat.png';
    document.querySelector('#image-dialog-form').requestSubmit();
    await new Promise(requestAnimationFrame);
  })()`);
  assert.equal(await evaluate("Boolean(document.querySelector('#preview-editor img[alt=cat]'))"), true,
    "preview image insert");
  const previewAfterImage = await evaluate("document.querySelector('#preview-editor').innerHTML");
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), previewBeforeImage,
    "preview image undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), previewAfterImage,
    "preview image redo");

  await setPreviewSelection("alpha", "#preview-editor p", true);
  const previewBeforeTable = await evaluate("document.querySelector('#preview-editor').innerHTML");
  await click('#table-size-grid [data-table-columns="2"][data-table-rows="2"]');
  assert.equal(await evaluate("document.querySelectorAll('#preview-editor table tbody tr').length"), 2,
    "preview table insert");
  const previewAfterTable = await evaluate("document.querySelector('#preview-editor').innerHTML");
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), previewBeforeTable,
    "preview table undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), previewAfterTable,
    "preview table redo");

  await evaluate(`(() => {
    const target = document.querySelector('#preview-editor table tbody td');
    const range = document.createRange(); range.selectNodeContents(target); range.collapse(true);
    const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
    target.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
  })()`);
  await click('[data-table-command="rowAfter"]');
  assert.equal(await evaluate("document.querySelectorAll('#preview-editor table tbody tr').length"), 3,
    "preview table row add");
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate("document.querySelectorAll('#preview-editor table tbody tr').length"), 2,
    "preview table row undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate("document.querySelectorAll('#preview-editor table tbody tr').length"), 3,
    "preview table row redo");

  await evaluate(`(() => {
    const target = document.querySelector('#preview-editor table tbody td');
    const range = document.createRange(); range.selectNodeContents(target); range.collapse(true);
    const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
    target.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
  })()`);
  await click('[data-table-command="columnAfter"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor table thead tr').cells.length"), 3,
    "preview table column add");
  await click('[data-editor-command="undo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor table thead tr').cells.length"), 2,
    "preview table column undo");
  await click('[data-editor-command="redo"]');
  assert.equal(await evaluate("document.querySelector('#preview-editor table thead tr').cells.length"), 3,
    "preview table column redo");

  await send("Emulation.setDeviceMetricsOverride", {
    width: 1000, height: 760, deviceScaleFactor: 1, mobile: false
  });
  await evaluate(`(() => {
    const target = document.querySelector('#preview-editor table tbody td');
    const range = document.createRange(); range.selectNodeContents(target); range.collapse(true);
    const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
    target.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
  })()`);
  const tableLayout = await evaluate(`(() => {
    const toolbar = document.querySelector('.toolbar').getBoundingClientRect();
    const actions = document.querySelector('#table-context-actions').getBoundingClientRect();
    const activeCellStyle = getComputedStyle(document.querySelector('#preview-editor .is-active-cell'));
    return {
      toolbarCenter: (toolbar.top + toolbar.bottom) / 2,
      actionsCenter: (actions.top + actions.bottom) / 2,
      actionsInside: actions.top >= toolbar.top && actions.bottom <= toolbar.bottom,
      activeCellOutline: activeCellStyle.outlineStyle,
      activeCellShadow: activeCellStyle.boxShadow
    };
  })()`);
  assert.ok(Math.abs(tableLayout.toolbarCenter - tableLayout.actionsCenter) <= 1,
    `table toolbar vertical alignment: ${JSON.stringify(tableLayout)}`);
  assert.equal(tableLayout.actionsInside, true, "table toolbar stays inside the main toolbar");
  assert.equal(tableLayout.activeCellOutline, "none", "active table cell has no overlapping outline");
  assert.notEqual(tableLayout.activeCellShadow, "none", "active table cell uses an inset highlight");

  const gridSelections = await evaluate(`(() => {
    const grid = document.querySelector('#table-size-grid');
    const inspect = (columns, rows) => {
      const button = grid.querySelector(
        '[data-table-columns="' + columns + '"][data-table-rows="' + rows + '"]');
      button.dispatchEvent(new PointerEvent('pointerover', { bubbles: true }));
      const selection = getComputedStyle(grid, '::after');
      const selectedCell = getComputedStyle(grid.querySelector('.is-selected'));
      return {
        width: selection.width,
        height: selection.height,
        rightBorder: selection.borderRightWidth,
        selectedCount: grid.querySelectorAll('[aria-selected="true"]').length,
        cellBorder: selectedCell.borderColor,
        cellShadow: selectedCell.boxShadow
      };
    };
    return { oneByOne: inspect(1, 1), twoByOne: inspect(2, 1), twoByTwo: inspect(2, 2) };
  })()`);
  assert.deepEqual(gridSelections.oneByOne,
    { width: "18px", height: "18px", rightBorder: "1px", selectedCount: 1,
      cellBorder: "rgba(0, 0, 0, 0)", cellShadow: "none" }, "1×1 table picker border");
  assert.deepEqual(gridSelections.twoByOne,
    { width: "41px", height: "18px", rightBorder: "1px", selectedCount: 2,
      cellBorder: "rgba(0, 0, 0, 0)", cellShadow: "none" }, "2×1 table picker border");
  assert.equal(gridSelections.twoByTwo.width, "41px", "2×2 table picker width");
  assert.equal(gridSelections.twoByTwo.height, "41px", "2×2 table picker height");

  const randomHistoryInitial = [
    "# History baseline",
    "",
    "alpha beta gamma",
    "",
    "- [ ] task item",
    "",
    "| Left | Right |",
    "| --- | --- |",
    "| one | two |"
  ].join("\n");
  const randomHistorySeeds = Array.from({ length: randomHistorySeedCount }, (_, index) =>
    (0x13579bdf + Math.imul(index, 0x9e3779b9)) >>> 0);
  let totalRandomOperations = 0;
  let totalRandomUndos = 0;
  let completedRandomSeeds = 0;
  for (const seed of randomHistorySeeds) {
    await loadCleanDocument(randomHistoryInitial);
    await setEditorMode("preview");
    const initialPreviewHtml = await evaluate("document.querySelector('#preview-editor').innerHTML");
    await setEditorMode("source");

    const randomRun = await evaluate(String.raw`(async () => {
      let randomState = ${seed} >>> 0;
      const random = () => {
        randomState = (Math.imul(randomState, 1664525) + 1013904223) >>> 0;
        return randomState / 0x100000000;
      };
      const pause = () => new Promise(requestAnimationFrame);
      const buttonClick = (selector) => {
        const button = document.querySelector(selector);
        if (!button || button.disabled) return false;
        button.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
        button.click();
        return true;
      };
      const ensureMode = async (mode) => {
        const sourceVisible = !document.querySelector('#source-pane').hidden;
        if ((mode === 'source') !== sourceVisible) {
          document.querySelector('[data-status-mode="' + mode + '"]').click();
          await pause();
        }
      };
      const selectSourceText = () => {
        const editor = document.querySelector('#source-editor');
        const match = /[A-Za-z]{2,}/g;
        const matches = [...editor.value.matchAll(match)];
        const selected = matches[Math.floor(random() * matches.length)] || { index: 0, 0: editor.value.slice(0, 1) };
        editor.focus();
        editor.setSelectionRange(selected.index, selected.index + selected[0].length);
      };
      const selectPreviewText = (wholeBlock = false) => {
        const editor = document.querySelector('#preview-editor');
        const candidates = [...editor.querySelectorAll('p, h1, h2, h3, li, td, th')]
          .filter((element) => element.textContent.trim());
        const target = candidates[Math.floor(random() * candidates.length)] || editor;
        const walker = document.createTreeWalker(target, NodeFilter.SHOW_TEXT);
        let text = walker.nextNode();
        while (text && !text.nodeValue.trim()) text = walker.nextNode();
        const range = document.createRange();
        if (wholeBlock || !text) range.selectNodeContents(target);
        else {
          const start = Math.min(text.nodeValue.search(/\S|$/), Math.max(0, text.length - 1));
          range.setStart(text, start);
          range.setEnd(text, Math.min(text.length, start + Math.max(1, Math.min(5, text.length - start))));
        }
        const selection = getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
        target.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
        return target;
      };
      const sourceType = (token) => {
        const editor = document.querySelector('#source-editor');
        const position = editor.value.length;
        editor.focus();
        editor.setSelectionRange(position, position);
        editor.dispatchEvent(new InputEvent('beforeinput', {
          bubbles: true, cancelable: true, inputType: 'insertText', data: token
        }));
        editor.setRangeText(token, position, position, 'end');
        editor.dispatchEvent(new InputEvent('input', {
          bubbles: true, inputType: 'insertText', data: token
        }));
      };
      const sourceDelete = () => {
        selectSourceText();
        const editor = document.querySelector('#source-editor');
        const start = editor.selectionStart;
        const end = editor.selectionEnd;
        editor.dispatchEvent(new InputEvent('beforeinput', {
          bubbles: true, cancelable: true, inputType: 'deleteContentForward'
        }));
        editor.setRangeText('', start, end, 'end');
        editor.dispatchEvent(new InputEvent('input', {
          bubbles: true, inputType: 'deleteContentForward'
        }));
      };
      const previewType = (token) => {
        const target = selectPreviewText(true);
        target.dispatchEvent(new InputEvent('beforeinput', {
          bubbles: true, cancelable: true, inputType: 'insertText', data: token
        }));
        target.append(document.createTextNode(token));
        target.dispatchEvent(new InputEvent('input', {
          bubbles: true, inputType: 'insertText', data: token
        }));
      };
      const operations = [];
      const operationKinds = Array.from({ length: 20 }, (_, index) => index);
      for (let round = 0; round < ${randomHistoryRounds}; round += 1) {
        const shuffled = [...operationKinds];
        for (let index = shuffled.length - 1; index > 0; index -= 1) {
          const swap = Math.floor(random() * (index + 1));
          [shuffled[index], shuffled[swap]] = [shuffled[swap], shuffled[index]];
        }
        for (const kind of shuffled) {
          if (kind <= 6 || kind === 15 || kind === 18) await ensureMode('source');
          else if (kind <= 14 || kind === 16 || kind === 19) await ensureMode('preview');
          if (kind === 0) sourceType('\nsource-' + round + '-' + Math.floor(random() * 1000));
          else if (kind === 1) { selectSourceText(); buttonClick('[data-format="bold"]'); }
          else if (kind === 2) { selectSourceText(); buttonClick('[data-format="italic"]'); }
          else if (kind === 3) { selectSourceText(); buttonClick('[data-format="strike"]'); }
          else if (kind === 4) { selectSourceText(); buttonClick('[data-format="inlineCode"]'); }
          else if (kind === 5) {
            const editor = document.querySelector('#source-editor');
            editor.setSelectionRange(editor.value.length, editor.value.length);
            buttonClick('#table-size-grid [data-table-columns="2"][data-table-rows="2"]');
          } else if (kind === 6) { selectSourceText(); buttonClick('[data-block-format="blockquote"]'); }
          else if (kind === 7) previewType(' preview-' + round);
          else if (kind === 8) { selectPreviewText(); buttonClick('[data-format="bold"]'); }
          else if (kind === 9) { selectPreviewText(); buttonClick('[data-format="italic"]'); }
          else if (kind === 10) { selectPreviewText(); buttonClick('[data-format="strike"]'); }
          else if (kind === 11) { selectPreviewText(true); buttonClick('[data-block-format="blockquote"]'); }
          else if (kind === 12) {
            const editor = document.querySelector('#preview-editor');
            const range = document.createRange();
            range.selectNodeContents(editor); range.collapse(false);
            const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
            editor.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
            buttonClick('#table-size-grid [data-table-columns="2"][data-table-rows="2"]');
          } else if (kind === 13) {
            const checkbox = document.querySelector('#preview-editor input[type="checkbox"]');
            if (checkbox) buttonClick('#preview-editor input[type="checkbox"]');
            else { selectPreviewText(); buttonClick('[data-format="inlineCode"]'); }
          } else if (kind === 14) {
            const cell = document.querySelector('#preview-editor tbody td');
            if (cell) {
              const range = document.createRange(); range.selectNodeContents(cell); range.collapse(true);
              const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
              cell.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
              buttonClick('[data-table-command="rowAfter"]');
            }
          } else if (kind === 15) {
            selectSourceText();
            buttonClick('[data-heading-level="' + (1 + Math.floor(random() * 3)) + '"]');
          } else if (kind === 16) {
            selectPreviewText(true);
            buttonClick('[data-block-format="horizontalRule"]');
          } else if (kind === 17) {
            const lf = document.querySelector('[data-menu-command="eol.lf"]');
            buttonClick(lf.getAttribute('aria-checked') === 'true'
              ? '[data-menu-command="eol.crlf"]' : '[data-menu-command="eol.lf"]');
          } else if (kind === 18) {
            sourceDelete();
          } else if (kind === 19) {
            selectPreviewText(true);
            buttonClick('[data-block-format="bulletList"]');
          }
          operations.push(kind);
        }
        await pause();
      }
      await ensureMode('source');
      const sourceAfterEdits = document.querySelector('#source-editor').value;
      let undoCount = 0;
      const undo = document.querySelector('[data-editor-command="undo"]');
      while (!undo.disabled && undoCount < 5000) {
        undo.click();
        undoCount += 1;
      }
      const sourceAfterFirstUndo = document.querySelector('#source-editor').value;
      let redoCount = 0;
      const redo = document.querySelector('[data-editor-command="redo"]');
      if (${randomHistoryRedoCycles}) {
        while (!redo.disabled && redoCount < 5000) {
          redo.click();
          redoCount += 1;
        }
      }
      const sourceAfterRedo = document.querySelector('#source-editor').value;
      let secondUndoCount = 0;
      if (${randomHistoryRedoCycles}) {
        while (!undo.disabled && secondUndoCount < 5000) {
          undo.click();
          secondUndoCount += 1;
        }
      }
      return {
        operations,
        undoCount,
        redoCount,
        secondUndoCount,
        sourceAfterEdits,
        sourceAfterFirstUndo,
        sourceAfterRedo,
        source: document.querySelector('#source-editor').value,
        dirty: !document.querySelector('#dirty-indicator').hidden,
        undoStillEnabled: !undo.disabled
      };
    })()`);
    assert.equal(randomRun.undoStillEnabled, false,
      `random history seed ${seed} exhausted undo buffer`);
    assert.equal(randomRun.sourceAfterFirstUndo, randomHistoryInitial,
      `random history seed ${seed} first undo pass restored exact source`);
    if (randomHistoryRedoCycles) {
      assert.equal(randomRun.sourceAfterRedo, randomRun.sourceAfterEdits,
        `random history seed ${seed} redo pass restored all randomized edits`);
      assert.equal(randomRun.redoCount, randomRun.undoCount,
        `random history seed ${seed} preserved every redo entry`);
      assert.equal(randomRun.secondUndoCount, randomRun.undoCount,
        `random history seed ${seed} preserved every second undo entry`);
    }
    assert.equal(randomRun.source, randomHistoryInitial,
      `random history seed ${seed} restored exact source after ${randomRun.undoCount} undos`);
    assert.equal(randomRun.dirty, false,
      `random history seed ${seed} restored clean document state`);
    await setEditorMode("preview");
    assert.equal(await evaluate("document.querySelector('#preview-editor').innerHTML"), initialPreviewHtml,
      `random history seed ${seed} left no preview DOM residue`);
    totalRandomOperations += randomRun.operations.length;
    totalRandomUndos += randomRun.undoCount;
    completedRandomSeeds += 1;
    if (randomHistorySeedCount > 3) {
      console.log(`Random history seed ${completedRandomSeeds}/${randomHistorySeedCount} passed ` +
        `(${randomRun.operations.length} operations, ${randomRun.undoCount} undo steps).`);
    }
  }

  console.log(`Random history passed: ${randomHistorySeeds.length} seeds, ` +
    `${randomHistoryRounds} rounds, ${totalRandomOperations} operations, ` +
    `${totalRandomUndos} undo steps.`);

  console.log("MdViewer UI smoke and randomized regression tests passed.");
} finally {
  try {
    await Promise.race([send("Browser.close").catch(() => undefined), delay(1000)]);
    await Promise.race([
      new Promise((resolveExit) => browser.once("exit", resolveExit)),
      delay(3000)
    ]);
  } catch { /* Continue with process cleanup. */ }
  try { socket?.close(); } catch { /* Ignore cleanup errors. */ }
  if (browser.exitCode === null && process.platform === "win32" && browser.pid) {
    const killer = spawn("taskkill", ["/PID", String(browser.pid), "/T", "/F"],
      { stdio: "ignore", windowsHide: true });
    await new Promise((resolveKill) => killer.once("exit", resolveKill));
  } else if (browser.exitCode === null) {
    browser.kill();
  }
  await new Promise((resolveClose) => server.close(resolveClose));
  for (let attempt = 0; attempt < 30; attempt += 1) {
    try {
      await rm(profile, { recursive: true, force: true });
      break;
    } catch (error) {
      if (attempt === 29) console.warn(`Could not remove temporary browser profile: ${error.message}`);
      else await delay(200);
    }
  }
}
