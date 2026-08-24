import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:net";
import { mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";
import { inflateRawSync } from "node:zlib";
import { tmpdir } from "node:os";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

if (process.platform !== "win32") {
  console.log("Native DOCX smoke test skipped: Windows only.");
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
    const flags = bytes.readUInt16LE(offset + 6);
    const method = bytes.readUInt16LE(offset + 8);
    const compressedSize = bytes.readUInt32LE(offset + 18);
    const expandedSize = bytes.readUInt32LE(offset + 22);
    const nameLength = bytes.readUInt16LE(offset + 26);
    const extraLength = bytes.readUInt16LE(offset + 28);
    assert.equal(flags & 0x08, 0, "DOCX ZIP uses known local sizes");
    const nameStart = offset + 30;
    const dataStart = nameStart + nameLength + extraLength;
    const name = bytes.subarray(nameStart, nameStart + nameLength).toString("utf8");
    const compressed = bytes.subarray(dataStart, dataStart + compressedSize);
    const value = method === 0 ? compressed : method === 8
      ? inflateRawSync(compressed) : null;
    assert.ok(value, `unsupported ZIP method for ${name}: ${method}`);
    assert.equal(value.length, expandedSize, `${name} expanded size`);
    entries.set(name, value);
    offset = dataStart + compressedSize;
  }
  return entries;
}

const temporaryRoot = await mkdtemp(join(tmpdir(), "mdviewer-docx-test-"));
if (dirname(resolve(temporaryRoot)) !== resolve(tmpdir()) ||
    !temporaryRoot.slice(temporaryRoot.lastIndexOf(sep) + 1)
      .startsWith("mdviewer-docx-test-")) {
  throw new Error(`Unsafe temporary DOCX test directory: ${temporaryRoot}`);
}

const markdownPath = join(temporaryRoot, "docx-export-smoke.md");
const docxPath = process.env.MDVIEWER_DOCX_TEST_OUTPUT
  ? resolve(process.env.MDVIEWER_DOCX_TEST_OUTPUT)
  : join(temporaryRoot, "docx-export-smoke.docx");
const pixel = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";
const repeated = Array.from({ length: 12 }, (_, index) => `
## 검증 섹션 ${index + 1}

편집 가능한 본문입니다. 긴 문장이 자연스럽게 줄바꿈되고 페이지 경계를 넘어가도 내용을 잃지 않아야 합니다. ` +
  `Word에서 제목, 본문, 표와 목록을 계속 편집할 수 있습니다. `.repeat(3) + `
`).join("\n");
await writeFile(markdownPath, `# DOCX 내보내기 검증

편집 가능한 **굵은 본문**, *기울임*, ~~취소선~~과 [공식 링크](https://example.com/docs?a=1&b=2)입니다.

1. 첫 번째 번호 목록
2. 두 번째 번호 목록
   - 중첩 글머리표

- [x] 완료한 작업
- [ ] 남은 작업

| 항목 | 값 | 설명 |
| --- | --- | --- |
| 용지 | A4 | 세로 방향 |
| 여백 | 20 mm | 일반 여백 |

> 인용문은 별도 Word 문단 스타일로 내보냅니다.

---

\`\`\`javascript
const answer = 42;
console.log("DOCX", answer);
\`\`\`

![내장 이미지](data:image/png;base64,${pixel})

${repeated}
`, "utf8");

const debuggingPort = await freePort();
const app = spawn(executable, [
  "--new-window", `--remote-debugging-port=${debuggingPort}`, markdownPath
], {
  stdio: "ignore", windowsHide: true,
  env: { ...process.env, MDVIEWER_DOCX_TEST_OUTPUT: docxPath }
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
  }, "DOCX test app did not expose its application page.");

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
    "document.querySelector('#document-name')?.textContent === 'docx-export-smoke.md'"),
  "DOCX source document did not open.");
  await evaluate(`(() => {
    localStorage.setItem('mdviewer.exportFormat', 'docx');
    document.querySelector('[data-menu-command="file.export"]').click();
  })()`);
  assert.equal(await evaluate("document.querySelector('#docx-paper-select').value"), "a4");
  assert.equal(await evaluate(
    "document.querySelector('input[name=\"docx-orientation\"]:checked').value"),
  "portrait");
  assert.equal(await evaluate("document.querySelector('#docx-margin-select').value"), "20");
  await evaluate("document.querySelector('#docx-export-save').click()");
  await waitFor(async () => {
    try { return (await stat(docxPath)).size > 1000; } catch { return false; }
  }, "Native DOCX export did not create a file.");

  const bytes = await readFile(docxPath);
  const entries = readZipEntries(bytes);
  for (const name of ["[Content_Types].xml", "_rels/.rels", "docProps/core.xml",
    "word/document.xml", "word/styles.xml", "word/numbering.xml",
    "word/settings.xml", "word/fontTable.xml", "word/_rels/document.xml.rels"]) {
    assert.ok(entries.has(name), `DOCX contains ${name}`);
  }
  const documentXml = entries.get("word/document.xml").toString("utf8");
  const stylesXml = entries.get("word/styles.xml").toString("utf8");
  const numberingXml = entries.get("word/numbering.xml").toString("utf8");
  const relationshipsXml = entries.get("word/_rels/document.xml.rels").toString("utf8");
  assert.match(documentXml, /DOCX 내보내기 검증/, "DOCX preserves Korean text");
  assert.match(documentXml, /<w:pStyle w:val="Heading1"\/>/, "DOCX contains heading styles");
  assert.match(documentXml, /<w:numPr>/, "DOCX contains editable lists");
  assert.match(documentXml, /<w:tbl>/, "DOCX contains an editable table");
  assert.match(documentXml, /<w:hyperlink r:id="rIdLink1"/, "DOCX contains a hyperlink");
  assert.match(documentXml, /<w:drawing>/, "DOCX contains an image drawing");
  assert.match(documentXml, /<w:pgSz w:w="11906" w:h="16838"\/>/,
    "DOCX contains A4 portrait page dimensions");
  assert.match(documentXml,
    /<w:pgMar w:top="1134" w:right="1134" w:bottom="1134" w:left="1134"/,
    "DOCX contains 20 mm margins");
  assert.match(stylesXml, /w:styleId="Code"/, "DOCX contains a code style");
  assert.match(numberingXml, /w:numFmt w:val="decimal"/,
    "DOCX contains decimal numbering");
  assert.match(relationshipsXml, /Target="https:\/\/example\.com\/docs\?a=1&amp;b=2"/,
    "DOCX escapes and preserves an external hyperlink target");
  assert.ok(entries.has("word/media/image1.png"), "DOCX embeds image bytes");
  console.log(`MdViewer native DOCX export smoke test passed (${bytes.length} bytes): ${docxPath}`);
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
