import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:net";
import { mkdir, mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

if (process.platform !== "win32") {
  console.log("Native print smoke test skipped: Windows only.");
  process.exit(0);
}

const workspace = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const executable = resolve(process.env.MDVIEWER_TEST_EXE ||
  `${workspace}\\x64\\Release\\MdViewer.exe`);
const delay = (milliseconds) => new Promise((resolveDelay) =>
  setTimeout(resolveDelay, milliseconds));

async function freePort() {
  const server = createServer();
  await new Promise((resolveListen) => server.listen(0, "127.0.0.1", resolveListen));
  const port = server.address().port;
  await new Promise((resolveClose) => server.close(resolveClose));
  return port;
}

async function waitFor(predicate, message, attempts = 240) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    const value = await predicate();
    if (value) return value;
    await delay(100);
  }
  throw new Error(message);
}

const temporaryRoot = await mkdtemp(join(tmpdir(), "mdviewer-print-test-"));
if (dirname(resolve(temporaryRoot)) !== resolve(tmpdir()) ||
    !temporaryRoot.slice(temporaryRoot.lastIndexOf(sep) + 1)
      .startsWith("mdviewer-print-test-")) {
  throw new Error(`Unsafe temporary print test directory: ${temporaryRoot}`);
}

const markdownPath = join(temporaryRoot, "print-range-smoke.md");
const sections = Array.from({ length: 24 }, (_, index) => `
## Print section ${index + 1}

This section verifies that page-range printing uses the exact visible preview. ` +
  `구간 인쇄는 지정한 페이지만 프린터로 전달해야 합니다. `.repeat(5) + `

| Page range | Section | Result |
| --- | --- | --- |
| 2-3 | ${index + 1} | Previewed before printing |

\`\`\`text
native-print-range-${index + 1}: abcdefghijklmnopqrstuvwxyz 0123456789
\`\`\`
`).join("\n");
await writeFile(markdownPath,
  `# MdViewer Print Range\n\nNative CEF print preview validation.\n${sections}`, "utf8");

const requestedOutput = process.env.MDVIEWER_PRINT_TEST_OUTPUT
  ? resolve(process.env.MDVIEWER_PRINT_TEST_OUTPUT) : null;
const requestedScreenshot = process.env.MDVIEWER_PRINT_TEST_SCREENSHOT
  ? resolve(process.env.MDVIEWER_PRINT_TEST_SCREENSHOT) : null;
const pdfOutput = requestedOutput || join(temporaryRoot, "print-range-preview.pdf");
await mkdir(dirname(pdfOutput), { recursive: true });
const debuggingPort = await freePort();
const app = spawn(executable, [
  "--new-window", `--remote-debugging-port=${debuggingPort}`, markdownPath
], {
  stdio: "ignore",
  windowsHide: true,
  env: {
    ...process.env,
    MDVIEWER_PRINT_TEST_OUTPUT: pdfOutput,
    MDVIEWER_PRINT_TEST_PRINTERS: "Test Office Printer|Test PDF Printer",
    MDVIEWER_PRINT_TEST_PROPERTIES: "apply"
  }
});

let socket;
let nextId = 1;
const pending = new Map();

function send(method, params = {}) {
  const id = nextId++;
  return new Promise((resolveMessage, rejectMessage) => {
    pending.set(id, { resolveMessage, rejectMessage });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

async function evaluate(expression) {
  const result = await send("Runtime.evaluate", {
    expression, awaitPromise: true, returnByValue: true
  });
  if (result.exceptionDetails) {
    throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  }
  return result.result.value;
}

try {
  const target = await waitFor(async () => {
    try {
      const targets = await (await fetch(
        `http://127.0.0.1:${debuggingPort}/json/list`)).json();
      return targets.find((entry) => entry.type === "page" &&
        entry.url.startsWith("https://app.mdviewer/"));
    } catch {
      return null;
    }
  }, "Print test app did not expose its application page.");

  socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolveOpen, rejectOpen) => {
    socket.addEventListener("open", resolveOpen, { once: true });
    socket.addEventListener("error", rejectOpen, { once: true });
  });
  socket.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (!message.id || !pending.has(message.id)) return;
    const request = pending.get(message.id);
    pending.delete(message.id);
    if (message.error) request.rejectMessage(new Error(message.error.message));
    else request.resolveMessage(message.result);
  });
  await send("Runtime.enable");
  await send("Page.enable");
  await waitFor(() => evaluate(
    "document.querySelector('#document-name')?.textContent === 'print-range-smoke.md'"),
  "Print source document did not open.");
  await evaluate(`(() => {
    window.__nativePrintMessages = [];
    window.addEventListener('mdviewerhostmessage', (event) => {
      window.__nativePrintMessages.push(event.detail);
    });
  })()`);

  await evaluate("document.querySelector('[data-menu-command=\"file.print\"]').click()");
  await waitFor(() => evaluate(
    "document.querySelector('#pdf-export-dialog').open && " +
    "document.querySelector('#pdf-preview-frame').src.includes('__pdf-preview?request=') && " +
    "document.querySelector('#pdf-printer-select').value === 'Test Office Printer'"),
  "All-pages print preview did not become ready.");
  const allPagesPreview = await evaluate(
    "document.querySelector('#pdf-preview-frame').src");

  await evaluate("document.querySelector('#pdf-printer-properties').click()");
  await waitFor(() => evaluate(
    "document.querySelector('#pdf-printer-properties-status').dataset.state === " +
    "'applied' && " +
    "!document.querySelector('#pdf-printer-properties').disabled"),
  "Advanced printer settings were not applied to the selected printer.");

  await evaluate(`(() => {
    const custom = document.querySelector(
      'input[name="pdf-print-pages"][value="custom"]');
    custom.checked = true;
    custom.dispatchEvent(new Event('change', { bubbles: true }));
    const range = document.querySelector('#pdf-page-range-input');
    range.value = '3-1';
    range.dispatchEvent(new Event('input', { bubbles: true }));
  })()`);
  await waitFor(() => evaluate(
    "document.querySelector('#pdf-page-range-input').getAttribute('aria-invalid') === 'true' && " +
    "document.querySelector('#pdf-export-save').disabled"),
  "Invalid print range was not rejected.");

  await evaluate(`(() => {
    const range = document.querySelector('#pdf-page-range-input');
    range.value = '2-3';
    range.dispatchEvent(new Event('input', { bubbles: true }));
  })()`);
  await waitFor(() => evaluate(
    `!document.querySelector('#pdf-export-save').disabled && ` +
    `document.querySelector('#pdf-preview-frame').src !== ${JSON.stringify(allPagesPreview)}`),
  "Pages 2-3 did not produce a ranged print preview.");
  assert.match(await evaluate(
    "document.querySelector('#pdf-paper-summary').textContent"), /2-3/,
  "print summary includes the selected page range");

  await evaluate(`(() => {
    const copies = document.querySelector('#pdf-print-copies');
    copies.value = '3';
    copies.dispatchEvent(new Event('input', { bubbles: true }));
  })()`);

  if (requestedScreenshot) {
    await delay(500);
    const screenshot = await send("Page.captureScreenshot", { format: "png" });
    await mkdir(dirname(requestedScreenshot), { recursive: true });
    await writeFile(requestedScreenshot, Buffer.from(screenshot.data, "base64"));
  }

  await evaluate("document.querySelector('#pdf-export-save').click()");
  await waitFor(async () => {
    try {
      return (await stat(pdfOutput)).size > 10_000;
    } catch {
      return false;
    }
  }, "Ranged preview was not passed to the native print path.");
  await waitFor(() => evaluate(
    "!document.querySelector('#pdf-export-dialog').open"),
  "First print dialog did not close after the direct print job was accepted.");

  const pdfBytes = await readFile(pdfOutput);
  assert.equal(pdfBytes.subarray(0, 5).toString("ascii"), "%PDF-",
    "native print path receives PDF preview bytes");
  const pageObjects = pdfBytes.toString("latin1").match(/\/Type\s*\/Page\b/g) || [];
  assert.equal(pageObjects.length, 2,
    "the printer receives only pages 2-3 from the full document");
  const printCompletion = await evaluate(
    "window.__nativePrintMessages.find(message => message.type === 'pdf.printed')");
  assert.equal(printCompletion.printerName, "Test Office Printer",
    "the native job uses the first dialog's selected printer");
  assert.equal(printCompletion.copies, 3,
    "the native job uses the first dialog's copy count");
  assert.equal(printCompletion.advancedSettings, true,
    "the native job retains the selected printer's advanced settings");

  console.log(`MdViewer native direct ranged print smoke test passed ` +
    `(${pdfBytes.length} bytes, 2 pages, 3 copies, advanced settings).`);
} finally {
  try { socket?.close(); } catch { /* Ignore cleanup errors. */ }
  try {
    if (app.exitCode === null) {
      spawnSync("taskkill.exe", ["/PID", String(app.pid), "/T", "/F"],
        { stdio: "ignore", windowsHide: true });
    }
  } catch { /* Best-effort process cleanup. */ }
  await rm(temporaryRoot, { recursive: true, force: true });
}
