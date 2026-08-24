import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:net";
import { mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";
import { inflateRawSync } from "node:zlib";
import { tmpdir } from "node:os";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

if (process.platform !== "win32") {
  console.log("Native HWPX smoke test skipped: Windows only.");
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

function readZipEntries(bytes) {
  const entries = new Map();
  let offset = 0;
  while (offset + 30 <= bytes.length && bytes.readUInt32LE(offset) === 0x04034b50) {
    const method = bytes.readUInt16LE(offset + 8);
    const compressedSize = bytes.readUInt32LE(offset + 18);
    const expandedSize = bytes.readUInt32LE(offset + 22);
    const nameLength = bytes.readUInt16LE(offset + 26);
    const extraLength = bytes.readUInt16LE(offset + 28);
    const nameStart = offset + 30;
    const dataStart = nameStart + nameLength + extraLength;
    const name = bytes.subarray(nameStart, nameStart + nameLength).toString("utf8");
    const compressed = bytes.subarray(dataStart, dataStart + compressedSize);
    const value = method === 0 ? compressed : method === 8
      ? inflateRawSync(compressed) : null;
    assert.ok(value, `unsupported ZIP method for ${name}: ${method}`);
    assert.equal(value.length, expandedSize, `${name} expanded size`);
    entries.set(name, { method, value });
    offset = dataStart + compressedSize;
  }
  return entries;
}

const temporaryRoot = await mkdtemp(join(tmpdir(), "mdviewer-hwpx-test-"));
if (dirname(resolve(temporaryRoot)) !== resolve(tmpdir()) ||
    !temporaryRoot.slice(temporaryRoot.lastIndexOf(sep) + 1)
      .startsWith("mdviewer-hwpx-test-")) {
  throw new Error(`Unsafe temporary HWPX test directory: ${temporaryRoot}`);
}

const markdownPath = join(temporaryRoot, "hwpx-export-smoke.md");
const hwpxPath = process.env.MDVIEWER_HWPX_TEST_OUTPUT
  ? resolve(process.env.MDVIEWER_HWPX_TEST_OUTPUT)
  : join(temporaryRoot, "hwpx-export-smoke.hwpx");
const pixel = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";
await writeFile(markdownPath, `# HWPX 내보내기

편집 가능한 **굵은 본문**과 [링크](https://example.com)입니다.

- 첫 번째 목록
- 두 번째 목록

| 항목 | 값 |
| --- | --- |
| 용지 | A4 |

\`\`\`javascript
const answer = 42;
\`\`\`

![내장 이미지](data:image/png;base64,${pixel})
`, "utf8");

const debuggingPort = await freePort();
const app = spawn(executable, [
  "--new-window", `--remote-debugging-port=${debuggingPort}`, markdownPath
], {
  stdio: "ignore", windowsHide: true,
  env: { ...process.env, MDVIEWER_HWPX_TEST_OUTPUT: hwpxPath }
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
  }, "HWPX test app did not expose its application page.");

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
  await waitFor(() => evaluate(
    "document.querySelector('#document-name')?.textContent === 'hwpx-export-smoke.md'"),
  "HWPX source document did not open.");
  await evaluate(`(() => {
    localStorage.setItem('mdviewer.exportFormat', 'hwpx');
    document.querySelector('[data-menu-command="file.export"]').click();
  })()`);
  assert.equal(await evaluate("document.querySelector('#hwpx-paper-select').value"), "a4");
  assert.equal(await evaluate(
    "document.querySelector('input[name=\"hwpx-orientation\"]:checked').value"),
  "portrait");
  await evaluate("document.querySelector('#hwpx-export-save').click()");
  await waitFor(async () => {
    try { return (await stat(hwpxPath)).size > 1000; } catch { return false; }
  }, "Native HWPX export did not create a file.");

  const bytes = await readFile(hwpxPath);
  const entries = readZipEntries(bytes);
  assert.equal(entries.get("mimetype")?.method, 0, "mimetype is stored");
  assert.equal(entries.get("mimetype")?.value.toString("utf8"), "application/hwp+zip");
  for (const name of ["Contents/header.xml", "Contents/section0.xml",
    "Contents/content.hpf", "META-INF/container.xml", "Preview/PrvText.txt"]) {
    assert.ok(entries.has(name), `HWPX contains ${name}`);
  }
  const section = entries.get("Contents/section0.xml").value.toString("utf8");
  assert.match(section, /HWPX 내보내기/, "HWPX preserves Korean text");
  assert.match(section, /<hp:tbl /, "HWPX preserves a table");
  assert.match(section, /<hp:pic /, "HWPX embeds an image shape");
  assert.ok([...entries.keys()].some((name) => name.startsWith("BinData/image")),
    "HWPX package embeds image bytes");
  console.log(`MdViewer native HWPX export smoke test passed (${bytes.length} bytes).`);
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
