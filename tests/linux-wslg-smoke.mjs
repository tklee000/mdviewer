import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const port = Number(process.env.MDVIEWER_DEBUGGING_PORT || 9223);
const screenshotPath = resolve(
  process.env.MDVIEWER_WSLG_SCREENSHOT || "out/linux-wslg-smoke.png"
);
const delay = (milliseconds) => new Promise((complete) => setTimeout(complete, milliseconds));

let socket;
let nextId = 1;
const pending = new Map();

for (let attempt = 0; attempt < 100; attempt += 1) {
  try {
    const response = await fetch(`http://127.0.0.1:${port}/json/list`);
    const targets = await response.json();
    const target = targets.find((entry) => entry.type === "page" && entry.url === "https://app.mdviewer/");
    if (target) {
      socket = new WebSocket(target.webSocketDebuggerUrl);
      await new Promise((open, reject) => {
        socket.addEventListener("open", open, { once: true });
        socket.addEventListener("error", reject, { once: true });
      });
      break;
    }
  } catch { /* WSLg CEF is still starting. */ }
  await delay(100);
}

assert(socket, `Could not find MdViewer on CEF debugging port ${port}.`);
socket.addEventListener("message", (event) => {
  const message = JSON.parse(event.data);
  if (!message.id || !pending.has(message.id)) return;
  const { resolveMessage, rejectMessage } = pending.get(message.id);
  pending.delete(message.id);
  if (message.error) rejectMessage(new Error(message.error.message));
  else resolveMessage(message.result);
});

function send(method, params = {}) {
  const id = nextId++;
  return new Promise((resolveMessage, rejectMessage) => {
    pending.set(id, { resolveMessage, rejectMessage });
    socket.send(JSON.stringify({ id, method, params }));
  });
}

async function evaluate(expression) {
  const result = await send("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true
  });
  if (result.exceptionDetails) {
    throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  }
  return result.result.value;
}

try {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (await evaluate("document.readyState === 'complete' && Boolean(document.querySelector('#source-editor'))")) {
      break;
    }
    await delay(100);
  }

  const state = await evaluate(`(() => {
    const shell = document.querySelector('.app-shell');
    const editor = document.querySelector('#source-editor');
    const preview = document.querySelector('#preview-editor');
    return {
      title: document.title,
      shell: Boolean(shell),
      editor: Boolean(editor),
      preview: Boolean(preview),
      nativeBridge: typeof window.mdViewerNative?.postMessage === 'function',
      editorText: editor?.value || '',
      visibleGoogleDriveItems: [...document.querySelectorAll(
        '[data-menu-command="file.openGoogleDrive"], [data-menu-command="file.saveGoogleDriveAs"]'
      )].filter((item) => !item.hidden).length,
      width: shell?.getBoundingClientRect().width || 0,
      height: shell?.getBoundingClientRect().height || 0
    };
  })()`);
  console.log(`WSLg state: ${JSON.stringify({
    ...state,
    editorText: `${state.editorText.length} characters`
  })}`);

  assert.equal(state.title, "MdViewer");
  assert.equal(state.shell, true);
  assert.equal(state.editor, true);
  assert.equal(state.preview, true);
  assert.equal(state.nativeBridge, true);
  assert.equal(state.visibleGoogleDriveItems, 0);
  if (process.env.MDVIEWER_EXPECT_TEXT) {
    assert(
      state.editorText.includes(process.env.MDVIEWER_EXPECT_TEXT),
      `The opened document did not contain ${JSON.stringify(process.env.MDVIEWER_EXPECT_TEXT)}.`
    );
  }
  assert(state.width >= 720, `Unexpected WSLg UI width: ${state.width}`);
  assert(state.height >= 460, `Unexpected WSLg UI height: ${state.height}`);

  const screenshot = await send("Page.captureScreenshot", { format: "png", fromSurface: true });
  await mkdir(dirname(screenshotPath), { recursive: true });
  await writeFile(screenshotPath, Buffer.from(screenshot.data, "base64"));
  console.log(`linux-wslg-smoke: ok (${state.width}x${state.height})`);
  console.log(`screenshot: ${screenshotPath}`);

  if (process.env.MDVIEWER_OPEN_SAVE_DIALOG === "1") {
    void send("Runtime.evaluate", {
      expression: `window.mdViewerNative.postMessage(JSON.stringify({
        type: "command",
        name: "file.saveAs"
      })); true`,
      returnByValue: true
    }).catch(() => {});
    await delay(500);
    console.log("linux-wslg-smoke: Save Document dialog requested");
  }
} finally {
  socket.close();
}
