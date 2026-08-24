import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:net";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

if (process.platform !== "win32") {
  console.log("Native PDF smoke test skipped: Windows only.");
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

async function waitFor(predicate, message, attempts = 200) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    const value = await predicate();
    if (value) return value;
    await delay(100);
  }
  throw new Error(message);
}

const temporaryRoot = await mkdtemp(join(tmpdir(), "mdviewer-pdf-test-"));
if (dirname(resolve(temporaryRoot)) !== resolve(tmpdir()) ||
    !temporaryRoot.slice(temporaryRoot.lastIndexOf(sep) + 1)
      .startsWith("mdviewer-pdf-test-")) {
  throw new Error(`Unsafe temporary PDF test directory: ${temporaryRoot}`);
}

const markdownPath = join(temporaryRoot, "pdf-export-smoke.md");
const sections = Array.from({ length: 18 }, (_, index) => `
## Section ${index + 1}

This paragraph checks pagination, wrapping, and readable body spacing. ` +
  `긴 한글 문장도 페이지 경계에서 잘리지 않고 자연스럽게 출력되어야 합니다. `.repeat(3) + `

| Column | Value | Notes |
| --- | --- | --- |
| ${index + 1} | A4 | Table rows stay together across pages. |

\`\`\`text
print-preview-${index + 1}: abcdefghijklmnopqrstuvwxyz 0123456789
\`\`\`
`).join("\n");
await writeFile(markdownPath,
  `# MdViewer PDF Export\n\nActual CEF PDF preview validation.\n${sections}`, "utf8");

const requestedOutput = process.env.MDVIEWER_PDF_TEST_OUTPUT
  ? resolve(process.env.MDVIEWER_PDF_TEST_OUTPUT) : null;
const requestedScreenshot = process.env.MDVIEWER_PDF_TEST_SCREENSHOT
  ? resolve(process.env.MDVIEWER_PDF_TEST_SCREENSHOT) : null;
const pdfOutput = requestedOutput || join(temporaryRoot, "pdf-export-preview.pdf");
const debuggingPort = await freePort();
const app = spawn(executable, [
  "--new-window", `--remote-debugging-port=${debuggingPort}`, markdownPath
], { stdio: "ignore", windowsHide: true });

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
  }, "PDF test app did not expose its application page.");

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
    "document.querySelector('#document-name')?.textContent === 'pdf-export-smoke.md'"),
  "PDF source document did not open.");

  await evaluate("document.querySelector('[data-menu-command=\"file.exportPdf\"]').click()");
  await waitFor(() => evaluate(
    "!document.querySelector('#pdf-export-save').disabled && " +
    "document.querySelector('#pdf-preview-frame').src.includes('__pdf-preview?request=')"),
  "A4 PDF preview did not become ready.");

  assert.equal(await evaluate("document.querySelector('#pdf-paper-select').value"), "a4");
  assert.equal(await evaluate(
    "document.querySelector('input[name=\"pdf-orientation\"]:checked').value"), "portrait");
  assert.equal(await evaluate("document.querySelector('#pdf-margin-select').value"), "20");

  if (process.env.MDVIEWER_PDF_TEST_PAGE_NUMBERS === "1") {
    const firstPreview = await evaluate("document.querySelector('#pdf-preview-frame').src");
    await evaluate(`(() => {
      const control = document.querySelector('#pdf-page-numbers');
      control.checked = true;
      control.dispatchEvent(new Event('change', { bubbles: true }));
    })()`);
    await waitFor(() => evaluate(
      `!document.querySelector('#pdf-export-save').disabled && ` +
      `document.querySelector('#pdf-preview-frame').src !== ${JSON.stringify(firstPreview)}`),
    "PDF preview did not refresh after enabling page numbers.");
  }

  if (process.env.MDVIEWER_PDF_TEST_LETTER_LANDSCAPE === "1") {
    const firstPreview = await evaluate("document.querySelector('#pdf-preview-frame').src");
    await evaluate(`(() => {
      const paper = document.querySelector('#pdf-paper-select');
      paper.value = 'letter';
      paper.dispatchEvent(new Event('change', { bubbles: true }));
      const orientation = document.querySelector(
        'input[name="pdf-orientation"][value="landscape"]');
      orientation.checked = true;
      orientation.dispatchEvent(new Event('change', { bubbles: true }));
      const margin = document.querySelector('#pdf-margin-select');
      margin.value = '10';
      margin.dispatchEvent(new Event('change', { bubbles: true }));
    })()`);
    await waitFor(() => evaluate(
      `!document.querySelector('#pdf-export-save').disabled && ` +
      `document.querySelector('#pdf-preview-frame').src !== ${JSON.stringify(firstPreview)}`),
    "PDF preview did not refresh for Letter landscape settings.");
  }

  const base64 = await evaluate(`(async () => {
    const url = document.querySelector('#pdf-preview-frame').src.split('#')[0];
    const response = await fetch(url, { cache: 'no-store' });
    if (!response.ok) throw new Error('Preview endpoint returned ' + response.status);
    const bytes = new Uint8Array(await response.arrayBuffer());
    let binary = '';
    for (let offset = 0; offset < bytes.length; offset += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    }
    return btoa(binary);
  })()`);
  const pdfBytes = Buffer.from(base64, "base64");
  assert.equal(pdfBytes.subarray(0, 5).toString("ascii"), "%PDF-",
    "preview endpoint returns a PDF");
  assert.ok(pdfBytes.length > 10_000, "multi-page PDF has substantial content");
  await mkdir(dirname(pdfOutput), { recursive: true });
  await writeFile(pdfOutput, pdfBytes);

  const frameTree = await send("Page.getFrameTree");
  assert.ok(frameTree.frameTree.childFrames?.length,
    "the generated PDF is attached to the in-app preview frame");

  if (requestedScreenshot) {
    await delay(1800);
    const screenshot = await send("Page.captureScreenshot", { format: "png" });
    await mkdir(dirname(requestedScreenshot), { recursive: true });
    await writeFile(requestedScreenshot, Buffer.from(screenshot.data, "base64"));
  }

  await evaluate("document.querySelector('[data-pdf-export-cancel]').click()");
  await waitFor(() => evaluate("!document.querySelector('#pdf-export-dialog').open"),
    "PDF preview dialog did not close cleanly.");
  const format = process.env.MDVIEWER_PDF_TEST_LETTER_LANDSCAPE === "1"
    ? "Letter landscape" : "A4 portrait";
  console.log(`MdViewer native ${format} PDF preview smoke test passed (${pdfBytes.length} bytes).`);
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
