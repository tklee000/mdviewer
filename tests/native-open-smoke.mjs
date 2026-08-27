import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:net";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

if (process.platform !== "win32") {
  console.log("Native open smoke test skipped: Windows only.");
  process.exit(0);
}

const workspace = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const executable = resolve(process.env.MDVIEWER_TEST_EXE ||
  `${workspace}\\x64\\Debug\\MdViewer.exe`);
const firstDocument = resolve(workspace, "tests", "roundtrip.md");
const secondDocument = resolve(workspace, "README.md");
const delay = (milliseconds) => new Promise((resolveDelay) =>
  setTimeout(resolveDelay, milliseconds));

async function freePort() {
  const server = createServer();
  await new Promise((resolveListen) => server.listen(0, "127.0.0.1", resolveListen));
  const port = server.address().port;
  await new Promise((resolveClose) => server.close(resolveClose));
  return port;
}

const nativeWindowSource = String.raw`
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class MdViewerWindowTest {
  public delegate bool EnumProc(IntPtr window, IntPtr data);
  [StructLayout(LayoutKind.Sequential)]
  public struct CopyData {
    public IntPtr dwData;
    public int cbData;
    public IntPtr lpData;
  }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int count);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, ref CopyData lParam, uint flags, uint timeout, out UIntPtr result);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

  public static IntPtr Find(uint expectedProcessId) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((window, data) => {
      uint processId;
      GetWindowThreadProcessId(window, out processId);
      var className = new StringBuilder(256);
      GetClassName(window, className, className.Capacity);
      if (processId == expectedProcessId && className.ToString() == "MdViewer.MainWindow") {
        found = window;
        return false;
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  public static string List() {
    var values = new List<string>();
    EnumWindows((window, data) => {
      var className = new StringBuilder(256);
      GetClassName(window, className, className.Capacity);
      if (className.ToString() != "MdViewer.MainWindow") return true;
      uint processId;
      GetWindowThreadProcessId(window, out processId);
      var title = new StringBuilder(1024);
      GetWindowText(window, title, title.Capacity);
      values.Add(processId + "|" + window.ToInt64() + "|" + title.ToString());
      return true;
    }, IntPtr.Zero);
    return string.Join("\n", values);
  }

  public static ulong Open(uint processId, string path) {
    var window = Find(processId);
    if (window == IntPtr.Zero) throw new InvalidOperationException("MdViewer window not found.");
    var allocated = Marshal.StringToHGlobalUni(path);
    try {
      var data = new CopyData {
        dwData = new IntPtr(0x4D445646),
        cbData = (path.Length + 1) * 2,
        lpData = allocated
      };
      UIntPtr result;
      if (SendMessageTimeout(window, 0x004A, IntPtr.Zero, ref data, 0x0003, 5000, out result) == IntPtr.Zero) {
        throw new InvalidOperationException("WM_COPYDATA failed: " + Marshal.GetLastWin32Error());
      }
      return result.ToUInt64();
    } finally {
      Marshal.FreeHGlobal(allocated);
    }
  }

  public static void Close(uint processId) {
    var window = Find(processId);
    if (window != IntPtr.Zero) PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
  }
}`;

function runWindowCommand(command) {
  const typeDefinition = Buffer.from(nativeWindowSource, "utf8").toString("base64");
  const script = `$typeDefinition = [Text.Encoding]::UTF8.GetString(` +
    `[Convert]::FromBase64String('${typeDefinition}')); ` +
    `Add-Type -TypeDefinition $typeDefinition; ${command}`;
  const encoded = Buffer.from(script, "utf16le").toString("base64");
  const result = spawnSync("powershell.exe",
    ["-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
    { encoding: "utf8", windowsHide: true });
  if (result.status !== 0) throw new Error(result.stderr || result.stdout);
  return result.stdout.trim();
}

function listWindows() {
  const output = runWindowCommand("[MdViewerWindowTest]::List()");
  if (!output) return [];
  return output.split(/\r?\n/).map((line) => {
    const [processId, handle, ...title] = line.split("|");
    return { processId: Number(processId), handle, title: title.join("|") };
  });
}

function sendOpen(processId, path) {
  return Number(runWindowCommand(
    `[MdViewerWindowTest]::Open(${processId}, ${JSON.stringify(path)})`));
}

function closeWindow(processId) {
  runWindowCommand(`[MdViewerWindowTest]::Close(${processId})`);
}

async function waitFor(predicate, message, attempts = 100) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    const value = await predicate();
    if (value) return value;
    await delay(100);
  }
  throw new Error(typeof message === "function" ? message() : message);
}

const debuggingPort = await freePort();
const windowsBefore = new Set(listWindows().map((window) => window.processId));
const app = spawn(executable, [
  "--new-window", `--remote-debugging-port=${debuggingPort}`, firstDocument
], { stdio: "ignore", windowsHide: true });
const testProcessIds = new Set([app.pid]);
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

async function evaluate(expression, userGesture = false) {
  const result = await send("Runtime.evaluate", {
    expression, awaitPromise: true, returnByValue: true, userGesture
  });
  if (result.exceptionDetails) {
    throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  }
  return result.result.value;
}

async function trustedClick(selector) {
  const point = await evaluate(`(() => {
    const bounds = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
    return { x: bounds.left + bounds.width / 2, y: bounds.top + bounds.height / 2 };
  })()`);
  await send("Input.dispatchMouseEvent", {
    type: "mousePressed", button: "left", buttons: 1,
    clickCount: 1, x: point.x, y: point.y
  });
  await send("Input.dispatchMouseEvent", {
    type: "mouseReleased", button: "left", buttons: 0,
    clickCount: 1, x: point.x, y: point.y
  });
}

try {
  const target = await waitFor(async () => {
    try {
      const targets = await (await fetch(
        `http://127.0.0.1:${debuggingPort}/json/list`)).json();
      return targets.find((entry) => entry.type === "page");
    } catch {
      return null;
    }
  }, "Debug MdViewer did not expose its test page.");
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
    "document.querySelector('#document-name')?.textContent === 'roundtrip.md'"),
  "Initial Markdown document did not open.");

  const initialSource = await evaluate("document.querySelector('#source-editor').value");
  await evaluate(`(async () => {
    document.querySelector('[data-mode-button="source"]').click();
    const editor = document.querySelector('#source-editor');
    editor.focus();
    editor.setSelectionRange(0, 0);
    await navigator.clipboard.writeText('native-context-paste');
    editor.dispatchEvent(new MouseEvent('contextmenu', {
      bubbles: true, cancelable: true, clientX: 80, clientY: 120
    }));
  })()`, true);
  await trustedClick('#editor-context-menu [data-editor-context-command="paste"]');
  await waitFor(() => evaluate(
    "document.querySelector('#source-editor').value.startsWith('native-context-paste')"),
  "Native CEF clipboard permission did not allow context-menu paste.");
  await evaluate("document.querySelector('[data-editor-command=\"undo\"]').click()");
  await waitFor(() => evaluate(
    `document.querySelector('#source-editor').value === ${JSON.stringify(initialSource)}`),
  "Undo did not restore the document after native context-menu paste.");

  assert.equal(sendOpen(app.pid, secondDocument), 2,
    "clean saved document should report that it launched a new window");
  let observedCleanWindows = [];
  const cleanNewWindow = await waitFor(() => {
    observedCleanWindows = listWindows();
    return observedCleanWindows.find((window) =>
      !windowsBefore.has(window.processId) && window.processId !== app.pid &&
      window.title.includes("README.md"));
  }, () => `Clean saved document did not launch a new MdViewer process. ` +
     `Observed windows: ${JSON.stringify(observedCleanWindows)}`);
  testProcessIds.add(cleanNewWindow.processId);
  assert.equal(await evaluate(
    "document.querySelector('#document-name').textContent"), "roundtrip.md",
  "clean saved document was replaced in the existing window");
  closeWindow(cleanNewWindow.processId);

  await evaluate(
    "document.querySelector('[data-menu-command=\"file.new\"]').click()");
  await waitFor(() => evaluate(
    "document.querySelector('#source-editor').value === '' && " +
    "document.querySelector('#dirty-indicator').hidden"),
  "New command did not create an empty document.");
  assert.equal(sendOpen(app.pid, secondDocument), 1,
    "empty new document should report that it reused the current window");
  await waitFor(() => evaluate(
    "document.querySelector('#document-name')?.textContent === 'README.md'"),
  "Empty new document did not open the forwarded document.");

  await evaluate(`(() => {
    document.querySelector('[data-mode-button="source"]').click();
    const editor = document.querySelector('#source-editor');
    editor.dispatchEvent(new InputEvent('beforeinput', {
      bubbles: true, cancelable: true, inputType: 'insertText', data: '\\nNative dirty-window smoke edit'
    }));
    editor.value += '\\nNative dirty-window smoke edit';
    editor.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertText' }));
  })()`);
  await waitFor(() => evaluate(
    "!document.querySelector('#dirty-indicator').hidden"),
  "Editor did not enter the dirty state.");
  await delay(250);
  assert.equal(sendOpen(app.pid, firstDocument), 2,
    "dirty window should report that it launched a new window");
  let observedWindows = [];
  const newWindow = await waitFor(() => {
    observedWindows = listWindows();
    const candidates = observedWindows.filter((window) =>
      !windowsBefore.has(window.processId) && window.processId !== app.pid &&
      window.title.includes("roundtrip.md"));
    return candidates[0];
  }, () => `Dirty window did not launch the document in a new MdViewer process. ` +
     `Observed windows: ${JSON.stringify(observedWindows)}`);
  testProcessIds.add(newWindow.processId);

  assert.equal(await evaluate(
    "document.querySelector('#document-name').textContent"), "README.md",
  "dirty source window kept its current document");
  assert.equal(await evaluate(
    "!document.querySelector('#dirty-indicator').hidden"), true,
  "dirty source window kept its unsaved edit");

  await evaluate("document.querySelector('[data-editor-command=\"undo\"]').click()");
  await waitFor(() => evaluate(
    "document.querySelector('#dirty-indicator').hidden"),
  "Undo did not restore the test document to a clean state.");
  await delay(250);
  closeWindow(newWindow.processId);
  closeWindow(app.pid);
  console.log("MdViewer native open policy smoke tests passed.");
} finally {
  try { socket?.close(); } catch { /* Ignore cleanup errors. */ }
  const isRunning = (processId) => {
    try {
      process.kill(processId, 0);
      return true;
    } catch {
      return false;
    }
  };
  for (let attempt = 0; attempt < 50; attempt += 1) {
    if ([...testProcessIds].every((processId) => !isRunning(processId))) break;
    await delay(100);
  }
  for (const processId of testProcessIds) {
    if (!isRunning(processId)) continue;
    const killer = spawnSync("taskkill.exe", ["/PID", String(processId), "/T", "/F"],
      { stdio: "ignore", windowsHide: true });
    if (![0, 128].includes(killer.status ?? 128)) {
      console.warn(`Could not clean up test process ${processId}.`);
    }
  }
}
