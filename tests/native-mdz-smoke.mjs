import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { deflateRawSync, inflateRawSync } from "node:zlib";

if (process.platform !== "win32") {
  console.log("Native MDZ smoke test skipped: Windows only.");
  process.exit(0);
}

const workspace = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const executable = resolve(process.env.MDVIEWER_TEST_EXE ||
  `${workspace}\\x64\\Debug\\MdViewer.exe`);
const delay = (milliseconds) => new Promise((resolveDelay) =>
  setTimeout(resolveDelay, milliseconds));

const crcTable = Array.from({ length: 256 }, (_, index) => {
  let value = index;
  for (let bit = 0; bit < 8; bit += 1) {
    value = (value >>> 1) ^ ((value & 1) ? 0xEDB88320 : 0);
  }
  return value >>> 0;
});

function crc32(bytes) {
  let value = 0xFFFFFFFF;
  for (const byte of bytes) value = crcTable[(value ^ byte) & 0xFF] ^ (value >>> 8);
  return (value ^ 0xFFFFFFFF) >>> 0;
}

function zip(entries) {
  const local = [];
  const central = [];
  let offset = 0;
  for (const [path, value] of Object.entries(entries).sort()) {
    const name = Buffer.from(path, "utf8");
    const data = Buffer.from(value);
    const compressed = deflateRawSync(data);
    const checksum = crc32(data);
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034B50, 0);
    header.writeUInt16LE(20, 4);
    header.writeUInt16LE(0x0800, 6);
    header.writeUInt16LE(8, 8);
    header.writeUInt32LE(checksum, 14);
    header.writeUInt32LE(compressed.length, 18);
    header.writeUInt32LE(data.length, 22);
    header.writeUInt16LE(name.length, 26);
    local.push(header, name, compressed);

    const directory = Buffer.alloc(46);
    directory.writeUInt32LE(0x02014B50, 0);
    directory.writeUInt16LE(20, 4);
    directory.writeUInt16LE(20, 6);
    directory.writeUInt16LE(0x0800, 8);
    directory.writeUInt16LE(8, 10);
    directory.writeUInt32LE(checksum, 16);
    directory.writeUInt32LE(compressed.length, 20);
    directory.writeUInt32LE(data.length, 24);
    directory.writeUInt16LE(name.length, 28);
    directory.writeUInt32LE(offset, 42);
    central.push(directory, name);
    offset += header.length + name.length + compressed.length;
  }
  const centralBytes = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054B50, 0);
  end.writeUInt16LE(Object.keys(entries).length, 8);
  end.writeUInt16LE(Object.keys(entries).length, 10);
  end.writeUInt32LE(centralBytes.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, centralBytes, end]);
}

function unzip(bytes) {
  let end = bytes.length - 22;
  while (end >= 0 && bytes.readUInt32LE(end) !== 0x06054B50) end -= 1;
  if (end < 0) throw new Error("ZIP end record missing");
  const count = bytes.readUInt16LE(end + 10);
  let cursor = bytes.readUInt32LE(end + 16);
  const entries = new Map();
  for (let index = 0; index < count; index += 1) {
    assert.equal(bytes.readUInt32LE(cursor), 0x02014B50, "central ZIP signature");
    const method = bytes.readUInt16LE(cursor + 10);
    const compressedSize = bytes.readUInt32LE(cursor + 20);
    const nameLength = bytes.readUInt16LE(cursor + 28);
    const extraLength = bytes.readUInt16LE(cursor + 30);
    const commentLength = bytes.readUInt16LE(cursor + 32);
    const localOffset = bytes.readUInt32LE(cursor + 42);
    const name = bytes.subarray(cursor + 46, cursor + 46 + nameLength).toString("utf8");
    const localNameLength = bytes.readUInt16LE(localOffset + 26);
    const localExtraLength = bytes.readUInt16LE(localOffset + 28);
    const dataOffset = localOffset + 30 + localNameLength + localExtraLength;
    const compressed = bytes.subarray(dataOffset, dataOffset + compressedSize);
    const data = method === 8 ? inflateRawSync(compressed) : Buffer.from(compressed);
    entries.set(name, { method, data });
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

async function freePort() {
  const server = createServer();
  await new Promise((resolveListen) => server.listen(0, "127.0.0.1", resolveListen));
  const port = server.address().port;
  await new Promise((resolveClose) => server.close(resolveClose));
  return port;
}

async function waitFor(predicate, message, attempts = 150) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (await predicate()) return;
    await delay(100);
  }
  throw new Error(message);
}

const temporaryRoot = await mkdtemp(join(tmpdir(), "mdviewer-mdz-test-"));
if (dirname(resolve(temporaryRoot)) !== resolve(tmpdir()) ||
    !temporaryRoot.slice(temporaryRoot.lastIndexOf(sep) + 1)
      .startsWith("mdviewer-mdz-test-")) {
  throw new Error(`Unsafe temporary MDZ test directory: ${temporaryRoot}`);
}
const mdzPath = join(temporaryRoot, "bundle.mdz");
const manifest = JSON.stringify({
  spec: { name: "mdzip-spec", version: "1.1.0" },
  title: "Native MDZ smoke", mode: "document", entryPoint: "index.md"
}, null, 2);
await writeFile(mdzPath, zip({ "index.md": "", "manifest.json": manifest }));

const debuggingPort = await freePort();
const app = spawn(executable, [
  "--new-window", `--remote-debugging-port=${debuggingPort}`, mdzPath
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
  let target;
  await waitFor(async () => {
    try {
      const targets = await (await fetch(
        `http://127.0.0.1:${debuggingPort}/json/list`)).json();
      target = targets.find((entry) => entry.type === "page");
      return Boolean(target);
    } catch {
      return false;
    }
  }, "MDZ test app did not expose a page");
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
    "document.querySelector('#encoding-status')?.textContent === 'MDZ · UTF-8'"),
  "Native app did not enter MDZ mode");

  const png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9Zz6cAAAAASUVORK5CYII=";
  await evaluate(`(async () => {
    document.querySelector('[data-status-mode="source"]').click();
    document.querySelector('[data-format="image"]').click();
    document.querySelector('#image-alt-input').value = 'pixel';
    document.querySelector('#image-source-input').value = 'data:image/png;base64,${png}';
    document.querySelector('#image-dialog-form').requestSubmit();
  })()`);
  await waitFor(() => evaluate(
    "document.querySelector('#source-editor').value === '![pixel](images/image.png)'"),
  "Native MDZ image link was not inserted");
  await evaluate("document.querySelector('[data-status-mode=\"preview\"]').click()");
  await waitFor(() => evaluate(`(() => {
    const image = document.querySelector('#preview-editor img[alt="pixel"]');
    return Boolean(image?.complete && image.naturalWidth === 1);
  })()`), "Preview did not load the image directly from the MDZ archive");
  await evaluate("document.querySelector('[data-status-mode=\"source\"]').click()");
  await evaluate("document.querySelector('[data-menu-command=\"file.save\"]').click()");
  await waitFor(() => evaluate("document.querySelector('#dirty-indicator').hidden"),
    "Native MDZ image save did not finish");
  let saved = unzip(await readFile(mdzPath));
  assert.equal(saved.get("images/image.png")?.method, 8,
    "saved MDZ image uses DEFLATE");
  assert.match(saved.get("index.md").data.toString("utf8"), /images\/image\.png/);

  await evaluate("document.querySelector('[data-editor-command=\"undo\"]').click()");
  await waitFor(() => evaluate("document.querySelector('#source-editor').value === ''"),
    "MDZ image undo did not restore initial source");
  await evaluate("document.querySelector('[data-menu-command=\"file.save\"]').click()");
  await waitFor(() => evaluate("document.querySelector('#dirty-indicator').hidden"),
    "MDZ undo save did not finish");
  saved = unzip(await readFile(mdzPath));
  assert.equal(saved.has("images/image.png"), false,
    "undo removes the newly packaged image from the saved MDZ");

  await evaluate("document.querySelector('[data-editor-command=\"redo\"]').click()");
  await waitFor(() => evaluate(
    "document.querySelector('#source-editor').value === '![pixel](images/image.png)'"),
  "MDZ image redo did not restore the link");
  await evaluate("document.querySelector('[data-menu-command=\"file.save\"]').click()");
  await waitFor(() => evaluate("document.querySelector('#dirty-indicator').hidden"),
    "MDZ redo save did not finish");
  saved = unzip(await readFile(mdzPath));
  assert.equal(saved.get("images/image.png")?.method, 8,
    "redo restores the compressed image entry");
  assert.equal([...saved.values()].every((entry) => entry.method === 8), true,
    "every MdViewer-written MDZ entry uses DEFLATE");
  console.log("MdViewer native MDZ save/image/undo/redo smoke tests passed.");
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
