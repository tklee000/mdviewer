(() => {
  "use strict";

  const i18n = window.MdViewerI18n;
  const requestedTheme = new URLSearchParams(location.search).get("theme") ||
    localStorage.getItem("mdviewer.theme") || "dark";
  document.documentElement.dataset.theme = requestedTheme === "light" ? "light" : "dark";
  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => [...document.querySelectorAll(selector)];
  const sourceEditor = $("#source-editor");
  const sourceHighlight = $("#source-highlight");
  const sourceHighlightCode = $("#source-highlight-code");
  const previewEditor = $("#preview-editor");
  const sourceFonts = Object.freeze({
    consolas: 'Consolas, "Noto Sans Mono", monospace',
    cascadia: '"Cascadia Code", Consolas, "Noto Sans Mono", monospace',
    courier: '"Courier New", Courier, monospace'
  });
  const previewFonts = Object.freeze({
    default: 'Inter, "Segoe UI", "Noto Sans", "Noto Sans CJK KR", "Malgun Gothic", sans-serif',
    segoe: '"Segoe UI", "Noto Sans CJK KR", "Malgun Gothic", sans-serif',
    arial: 'Arial, "Noto Sans CJK KR", "Malgun Gothic", sans-serif',
    georgia: 'Georgia, "Times New Roman", serif'
  });
  const sourceFontSizes = Object.freeze([12, 14, 15, 16, 18, 20, 24]);
  const previewFontSizes = Object.freeze([14, 16, 18, 20, 22, 24]);
  const storedChoice = (key, choices, fallback) => {
    const value = localStorage.getItem(key);
    return Object.hasOwn(choices, value) ? value : fallback;
  };
  const storedSize = (key, choices, fallback) => {
    const value = Number(localStorage.getItem(key));
    return choices.includes(value) ? value : fallback;
  };
  const storedHeadingLevel = Number(localStorage.getItem("mdviewer.lastHeadingLevel"));

  const state = {
    text: "",
    savedText: "",
    mode: "preview",
    activeEditor: "preview",
    path: "",
    origin: "local",
    format: "markdown",
    name: "Untitled",
    dirty: false,
    encoding: "UTF-8",
    eol: "CRLF",
    savedEol: "CRLF",
    theme: document.documentElement.dataset.theme,
    sourceFont: storedChoice("mdviewer.sourceFont", sourceFonts, "consolas"),
    sourceFontSize: storedSize("mdviewer.sourceFontSize", sourceFontSizes, 15),
    previewFont: storedChoice("mdviewer.previewFont", previewFonts, "default"),
    previewFontSize: storedSize("mdviewer.previewFontSize", previewFontSizes, 16),
    lastHeadingLevel: Number.isInteger(storedHeadingLevel) && storedHeadingLevel >= 1 && storedHeadingLevel <= 6
      ? storedHeadingLevel : 2,
    hidePreviewSpelling: localStorage.getItem("mdviewer.hidePreviewSpelling") === "true",
    splitRatio: Math.min(75, Math.max(25,
      Number(localStorage.getItem("mdviewer.splitRatio")) || 50)),
    previewChanged: false,
    googleDriveBusy: false,
    applying: false
  };
  let previewSelectionRange = null;
  const documentUndoHistory = [];
  const documentRedoHistory = [];
  const maximumDocumentHistory = 2000;
  let applyingSourceFormatting = false;
  let applyingPreviewFormatting = false;
  let sourceBeforeInputRecorded = false;
  let splitScrollFrame = 0;
  let synchronizingSplitScroll = false;
  let pendingImageSourceSelection = null;
  let pendingImageFileName = "";
  let recentDocumentItems = [];
  let toastHideTimer = 0;
  let toastRemoveTimer = 0;
  const splitMinimumWidth = 860;

  function activeEditorMode() {
    return state.mode === "split" ? state.activeEditor : state.mode;
  }

  function setActiveEditor(mode) {
    if (!['source', 'preview'].includes(mode)) return;
    state.activeEditor = mode;
    if (state.mode === "split") {
      $(".editor-stage").dataset.activeEditor = mode;
      updateChrome();
    }
  }

  function post(type, payload = {}) {
    const bridge = window.mdViewerNative;
    if (!bridge?.postMessage) return false;
    bridge.postMessage(JSON.stringify({ type, ...payload }));
    return true;
  }

  function showToast(message, tone = "info", title = "") {
    const region = $("#toast-region");
    const tones = {
      success: { icon: "✓", duration: 2800 },
      info: { icon: "i", duration: 3800 },
      warning: { icon: "!", duration: 5000 },
      error: { icon: "×", duration: 6000 }
    };
    const normalizedTone = Object.hasOwn(tones, tone) ? tone : "info";
    const presentation = tones[normalizedTone];
    clearTimeout(toastHideTimer);
    clearTimeout(toastRemoveTimer);
    const toast = document.createElement("div");
    toast.className = `toast-notification ${normalizedTone}`;
    toast.setAttribute("role", ["warning", "error"].includes(normalizedTone) ? "alert" : "status");
    const icon = document.createElement("span");
    icon.className = "toast-icon";
    icon.setAttribute("aria-hidden", "true");
    icon.textContent = presentation.icon;
    const content = document.createElement("span");
    content.className = "toast-content";
    if (title) {
      const heading = document.createElement("span");
      heading.className = "toast-title";
      heading.textContent = title;
      content.append(heading);
    }
    const text = document.createElement("span");
    text.className = "toast-message";
    text.textContent = message;
    content.append(text);
    toast.append(icon, content);
    region.replaceChildren(toast);
    requestAnimationFrame(() => toast.classList.add("is-visible"));
    toastHideTimer = setTimeout(() => {
      toast.classList.remove("is-visible");
      toastRemoveTimer = setTimeout(() => {
        if (region.firstElementChild === toast) region.replaceChildren();
      }, 180);
    }, presentation.duration);
  }

  function applyTheme(theme) {
    state.theme = theme === "light" ? "light" : "dark";
    document.documentElement.dataset.theme = state.theme;
    localStorage.setItem("mdviewer.theme", state.theme);
    $("#theme-button").textContent = state.theme === "dark" ? "☀" : "◐";
    $("#theme-button").dataset.i18nTitle = state.theme === "dark" ? "Use light theme" : "Use dark theme";
    $("#theme-button").dataset.i18nAriaLabel = state.theme === "dark" ? "Use light theme" : "Use dark theme";
    i18n.localizeDocument($("#theme-button").parentElement);
    $$('[data-theme-menu]').forEach((item) =>
      item.setAttribute("aria-checked", String(item.dataset.themeMenu === state.theme)));
  }

  function applyEditorPreferences() {
    const rootStyle = document.documentElement.style;
    rootStyle.setProperty("--source-font-family", sourceFonts[state.sourceFont]);
    rootStyle.setProperty("--source-font-size", `${state.sourceFontSize}px`);
    rootStyle.setProperty("--preview-font-family", previewFonts[state.previewFont]);
    rootStyle.setProperty("--preview-font-size", `${state.previewFontSize}px`);
    previewEditor.setAttribute("spellcheck", String(!state.hidePreviewSpelling));
    updateSourceHighlight();

    localStorage.setItem("mdviewer.sourceFont", state.sourceFont);
    localStorage.setItem("mdviewer.sourceFontSize", String(state.sourceFontSize));
    localStorage.setItem("mdviewer.previewFont", state.previewFont);
    localStorage.setItem("mdviewer.previewFontSize", String(state.previewFontSize));
    localStorage.setItem("mdviewer.hidePreviewSpelling", String(state.hidePreviewSpelling));

    $("#preview-spelling-menu").setAttribute(
      "aria-checked", String(state.hidePreviewSpelling));
  }

  function openFontSettings() {
    $("#source-font-select").value = state.sourceFont;
    $("#source-font-size-select").value = String(state.sourceFontSize);
    $("#preview-font-select").value = state.previewFont;
    $("#preview-font-size-select").value = String(state.previewFontSize);
    $("#font-settings-dialog").showModal();
    $("#source-font-select").focus();
  }

  function applyFontSettings() {
    const sourceFont = $("#source-font-select").value;
    const sourceFontSize = Number($("#source-font-size-select").value);
    const previewFont = $("#preview-font-select").value;
    const previewFontSize = Number($("#preview-font-size-select").value);
    if (!Object.hasOwn(sourceFonts, sourceFont) || !sourceFontSizes.includes(sourceFontSize) ||
        !Object.hasOwn(previewFonts, previewFont) || !previewFontSizes.includes(previewFontSize)) return;
    state.sourceFont = sourceFont;
    state.sourceFontSize = sourceFontSize;
    state.previewFont = previewFont;
    state.previewFontSize = previewFontSize;
    applyEditorPreferences();
  }

  function sourceHeadingLevel() {
    const start = sourceEditor.selectionStart;
    const lineStart = sourceEditor.value.lastIndexOf("\n", start - 1) + 1;
    const lineEndIndex = sourceEditor.value.indexOf("\n", start);
    const lineEnd = lineEndIndex < 0 ? sourceEditor.value.length : lineEndIndex;
    const heading = sourceEditor.value.slice(lineStart, lineEnd).match(/^ {0,3}(#{1,6})(?:[ \t]+|$)/);
    return heading ? heading[1].length : 0;
  }

  function rememberPreviewSelection() {
    const selection = window.getSelection();
    if (!selection?.rangeCount) return;
    const range = selection.getRangeAt(0);
    if (previewEditor.contains(range.commonAncestorContainer)) previewSelectionRange = range.cloneRange();
  }

  function restorePreviewSelection() {
    previewEditor.focus();
    if (!previewSelectionRange) return;
    const selection = window.getSelection();
    try {
      selection.removeAllRanges();
      selection.addRange(previewSelectionRange);
    } catch {
      previewSelectionRange = null;
    }
  }

  function previewSelectionElement() {
    const selection = window.getSelection();
    let node = selection?.rangeCount ? selection.anchorNode : null;
    if (!node || !previewEditor.contains(node)) node = previewSelectionRange?.startContainer || null;
    return node?.nodeType === Node.ELEMENT_NODE ? node : node?.parentElement;
  }

  function previewHeadingLevel() {
    const selection = window.getSelection();
    let node = selection?.rangeCount ? selection.anchorNode : null;
    if (!node || !previewEditor.contains(node)) node = previewSelectionRange?.startContainer || null;
    const element = node?.nodeType === Node.ELEMENT_NODE ? node : node?.parentElement;
    const heading = element?.closest?.("h1, h2, h3, h4, h5, h6");
    return heading && previewEditor.contains(heading) ? Number(heading.tagName[1]) : 0;
  }

  function updateHeadingChrome() {
    const currentLevel = activeEditorMode() === "source" ? sourceHeadingLevel() : previewHeadingLevel();
    $("#heading-button-label").textContent = `H${state.lastHeadingLevel}`;
    $$('[data-heading-level]').forEach((item) => {
      const level = Number(item.dataset.headingLevel);
      item.textContent = level === 0 ? i18n.t("Paragraph") : `${i18n.t("Heading")} ${level}`;
      item.setAttribute("aria-checked", String(level === currentLevel));
    });
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (character) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;"
    })[character]);
  }

  function highlightSourceInline(value) {
    const pattern = /(`+)([^`\n]*?)\1|(!?\[[^\]\n]*\]\([^\)\n]+\))|(\*\*[^*\n]+?\*\*|__[^_\n]+?__|~~[^~\n]+?~~)|(\*[^*\n]+?\*|_[^_\n]+?_)/g;
    let output = "";
    let offset = 0;
    for (const match of String(value).matchAll(pattern)) {
      output += escapeHtml(value.slice(offset, match.index));
      const token = match[0];
      let className = "source-syntax-emphasis";
      if (match[1]) className = "source-syntax-code";
      else if (match[3]) className = "source-syntax-link";
      else if (match[4]) className = "source-syntax-strong";
      output += `<span class="${className}">${escapeHtml(token)}</span>`;
      offset = match.index + token.length;
    }
    return output + escapeHtml(value.slice(offset));
  }

  function renderSourceHighlight(value) {
    let fence = null;
    return String(value).split("\n").map((line) => {
      const fenceLine = line.match(/^([ \t]{0,3})(`{3,}|~{3,})(.*)$/);
      if (fenceLine) {
        const marker = fenceLine[2];
        const closing = fence && marker[0] === fence.character && marker.length >= fence.length;
        if (closing) fence = null;
        else if (!fence) fence = { character: marker[0], length: marker.length };
        return `<span class="source-syntax-fence">${escapeHtml(line)}</span>`;
      }
      if (fence || /^(?: {4}|\t)/.test(line)) {
        return `<span class="source-syntax-fence">${escapeHtml(line)}</span>`;
      }

      const heading = line.match(/^([ \t]{0,3})(#{1,6})([ \t]+)(.*)$/);
      if (heading) {
        return `${escapeHtml(heading[1])}<span class="source-syntax-marker">${heading[2]}</span>${escapeHtml(heading[3])}` +
          `<span class="source-syntax-heading">${highlightSourceInline(heading[4])}</span>`;
      }
      const quote = line.match(/^([ \t]{0,3}(?:>[ \t]?)+)(.*)$/);
      if (quote) {
        return `<span class="source-syntax-marker">${escapeHtml(quote[1])}</span>` +
          `<span class="source-syntax-quote">${highlightSourceInline(quote[2])}</span>`;
      }
      const list = line.match(/^([ \t]*)([-+*]|\d+\.)([ \t]+)(.*)$/);
      if (list) {
        return `${escapeHtml(list[1])}<span class="source-syntax-marker">${escapeHtml(list[2])}</span>` +
          `${escapeHtml(list[3])}${highlightSourceInline(list[4])}`;
      }
      if (/^[ \t]{0,3}(?:\*(?:[ \t]*\*){2,}|-(?:[ \t]*-){2,}|_(?:[ \t]*_){2,})[ \t]*$/.test(line)) {
        return `<span class="source-syntax-marker">${escapeHtml(line)}</span>`;
      }
      return highlightSourceInline(line);
    }).join("\n");
  }

  function syncSourceHighlightScroll() {
    sourceHighlight.scrollTop = sourceEditor.scrollTop;
    sourceHighlight.scrollLeft = sourceEditor.scrollLeft;
  }

  function updateSourceHighlight() {
    const trailingLine = sourceEditor.value.endsWith("\n") ? " " : "";
    sourceHighlightCode.innerHTML = renderSourceHighlight(sourceEditor.value) + trailingLine;
    syncSourceHighlightScroll();
  }

  function safeLinkUrl(value) {
    const url = String(value || "").trim();
    if (/^(https?:|mailto:|#)/i.test(url)) return url;
    if (/^(javascript:|vbscript:|data:)/i.test(url)) return "";
    return url;
  }

  function safeImageUrl(value) {
    const url = String(value || "").trim();
    if (/^data:image\/(png|jpeg|gif|webp|bmp);base64,/i.test(url)) return url;
    if (/^https?:\/\//i.test(url)) return url;
    if (/^[a-z][a-z0-9+.-]*:/i.test(url) || url.startsWith("//") || url.startsWith("/")) return "";
    if (!url) return "";
    let localPath = url.replaceAll("\\", "/");
    try { localPath = decodeURIComponent(localPath); } catch { /* Keep the literal path. */ }
    return `https://app.mdviewer/__asset?path=${encodeURIComponent(localPath)}`;
  }

  function renderInline(value) {
    const tokens = [];
    const hold = (html) => `\u0000${tokens.push(html) - 1}\u0000`;
    let text = String(value || "");

    text = text.replace(/`([^`\n]+)`/g, (_, code) =>
      hold(`<code>${escapeHtml(code)}</code>`));
    text = text.replace(/!\[((?:\\.|[^\]\\])*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g,
      (_, escapedAlt, rawUrl) => {
        const alt = escapedAlt.replace(/\\([\\\]])/g, "$1");
        const source = safeImageUrl(rawUrl);
        if (!source) return `![${alt}](${rawUrl})`;
        return hold(`<img src="${escapeHtml(source)}" data-md-src="${escapeHtml(rawUrl)}" alt="${escapeHtml(alt)}">`);
      });
    text = text.replace(/\[([^\]]+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g,
      (_, label, rawUrl) => {
        const href = safeLinkUrl(rawUrl);
        if (!href) return `[${label}](${rawUrl})`;
        return hold(`<a href="${escapeHtml(href)}" data-md-href="${escapeHtml(rawUrl)}">${escapeHtml(label)}</a>`);
      });

    text = escapeHtml(text);
    text = text.replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>");
    text = text.replace(/__([^_\n]+)__/g, "<strong>$1</strong>");
    text = text.replace(/~~([^~\n]+)~~/g, "<del>$1</del>");
    text = text.replace(/(^|[^*])\*([^*\n]+)\*/g, "$1<em>$2</em>");
    text = text.replace(/(^|[^_])_([^_\n]+)_/g, "$1<em>$2</em>");
    text = text.replace(/ {2}\n/g, "<br>");
    return text.replace(/\u0000(\d+)\u0000/g, (_, index) => tokens[Number(index)] || "");
  }

  function splitTableRow(line) {
    let value = line.trim();
    if (value.startsWith("|")) value = value.slice(1);
    if (value.endsWith("|")) value = value.slice(0, -1);
    return value.split("|").map((cell) => cell.trim());
  }

  function isTableDivider(line) {
    const cells = splitTableRow(line);
    return cells.length > 0 && cells.every((cell) => /^:?-{3,}:?$/.test(cell));
  }

  function isBlockStart(lines, index) {
    const line = lines[index] || "";
    return /^ {0,3}(#{1,6})\s+/.test(line) || /^ {0,3}(```|~~~)/.test(line) ||
      /^\s*(?:[-+*]|\d+\.)\s+/.test(line) || /^\s*>/.test(line) ||
      /^\s*((\*\s*){3,}|(-\s*){3,}|(_\s*){3,})$/.test(line) ||
      (index + 1 < lines.length && line.includes("|") && isTableDivider(lines[index + 1]));
  }

  function listLineDetails(line) {
    const match = String(line).match(/^(\s*)([-+*]|\d+\.)\s+(.+)$/);
    if (!match) return null;
    const indentation = match[1].replaceAll("\t", "    ").length;
    return { indentation, ordered: /\d+\./.test(match[2]), content: match[3] };
  }

  function renderListLevel(lines, startIndex, indentation) {
    const first = listLineDetails(lines[startIndex]);
    if (!first || first.indentation !== indentation) return null;
    const ordered = first.ordered;
    const tag = ordered ? "ol" : "ul";
    const items = [];
    let index = startIndex;

    while (index < lines.length) {
      const details = listLineDetails(lines[index]);
      if (!details || details.indentation < indentation) break;
      if (details.indentation > indentation) {
        if (!items.length) break;
        const nested = renderListLevel(lines, index, details.indentation);
        if (!nested) break;
        items[items.length - 1].nested.push(nested.html);
        index = nested.nextIndex;
        continue;
      }
      if (details.ordered !== ordered) break;

      const task = details.content.match(/^\[([ xX])\]\s*(.*)$/);
      items.push({
        html: task
          ? `<input type="checkbox" contenteditable="false"${task[1].toLowerCase() === "x" ? " checked" : ""}>${renderInline(task[2])}`
          : renderInline(details.content),
        task: Boolean(task),
        nested: []
      });
      index += 1;
    }

    return {
      html: `<${tag}>${items.map((item) =>
        `<li${item.task ? ' data-task="true"' : ""}>${item.html}${item.nested.join("")}</li>`).join("")}</${tag}>`,
      nextIndex: index
    };
  }

  function withSourceRange(html, start, end = start) {
    return html.replace(/^<([a-z][\w-]*)/i,
      `<$1 data-source-start="${start}" data-source-end="${end}"`);
  }

  function renderMarkdown(markdown) {
    const lines = String(markdown || "").replaceAll("\r\n", "\n").split("\n");
    const output = [];
    let index = 0;

    if (lines[0]?.trim() === "---") {
      const end = lines.slice(1).findIndex((line) => line.trim() === "---");
      if (end >= 0) {
        const last = end + 1;
        const raw = lines.slice(0, last + 1).join("\n");
        output.push(withSourceRange(
          `<div class="protected-block" contenteditable="false" data-raw="${escapeHtml(raw)}">${escapeHtml(raw)}</div>`,
          0, last));
        index = last + 1;
      }
    }

    while (index < lines.length) {
      const line = lines[index];
      if (!line.trim()) { index += 1; continue; }

      const fence = line.match(/^ {0,3}(```|~~~)\s*([^\s]*)\s*$/);
      if (fence) {
        const start = index;
        const marker = fence[1];
        const language = fence[2] || "";
        const code = [];
        index += 1;
        while (index < lines.length && !new RegExp(`^ {0,3}${marker}`).test(lines[index])) {
          code.push(lines[index]);
          index += 1;
        }
        if (index < lines.length) index += 1;
        output.push(withSourceRange(
          `<pre data-language="${escapeHtml(language)}"><code>${escapeHtml(code.join("\n"))}</code></pre>`,
          start, index - 1));
        continue;
      }

      const heading = line.match(/^ {0,3}(#{1,6})\s+(.+?)\s*#*$/);
      if (heading) {
        const level = heading[1].length;
        output.push(withSourceRange(`<h${level}>${renderInline(heading[2])}</h${level}>`, index));
        index += 1;
        continue;
      }

      if (/^\s*((\*\s*){3,}|(-\s*){3,}|(_\s*){3,})$/.test(line)) {
        output.push(withSourceRange("<hr>", index));
        index += 1;
        continue;
      }

      if (index + 1 < lines.length && line.includes("|") && isTableDivider(lines[index + 1])) {
        const start = index;
        const headers = splitTableRow(line);
        const rows = [];
        index += 2;
        while (index < lines.length && lines[index].includes("|") && lines[index].trim()) {
          rows.push(splitTableRow(lines[index]));
          index += 1;
        }
        output.push(withSourceRange(
          `<table><thead><tr>${headers.map((cell) => `<th>${renderInline(cell)}</th>`).join("")}</tr></thead>` +
          `<tbody>${rows.map((row) => `<tr>${headers.map((_, column) => `<td>${renderInline(row[column] || "")}</td>`).join("")}</tr>`).join("")}</tbody></table>`,
          start, index - 1));
        continue;
      }

      if (/^\s*>/.test(line)) {
        const start = index;
        const quoted = [];
        while (index < lines.length && /^\s*>/.test(lines[index])) {
          quoted.push(lines[index].replace(/^\s*>\s?/, ""));
          index += 1;
        }
        output.push(withSourceRange(
          `<blockquote>${renderMarkdown(quoted.join("\n"))}</blockquote>`, start, index - 1));
        continue;
      }

      const listMatch = listLineDetails(line);
      if (listMatch) {
        const start = index;
        const renderedList = renderListLevel(lines, index, listMatch.indentation);
        index = renderedList.nextIndex;
        output.push(withSourceRange(renderedList.html, start, index - 1));
        continue;
      }

      const start = index;
      const paragraph = [line.trim()];
      index += 1;
      while (index < lines.length && lines[index].trim() && !isBlockStart(lines, index)) {
        paragraph.push(lines[index].trim());
        index += 1;
      }
      output.push(withSourceRange(`<p>${renderInline(paragraph.join("\n"))}</p>`, start, index - 1));
    }
    return output.join("\n");
  }

  function inlineToMarkdown(node) {
    if (node.nodeType === Node.TEXT_NODE) return node.nodeValue || "";
    if (node.nodeType !== Node.ELEMENT_NODE) return "";
    const element = node;
    const content = () => [...element.childNodes].map(inlineToMarkdown).join("");
    switch (element.tagName.toLowerCase()) {
      case "br": return "  \n";
      case "strong": case "b": return `**${content()}**`;
      case "em": case "i": return `*${content()}*`;
      case "del": case "s": case "strike": return `~~${content()}~~`;
      case "code": return `\`${content().replaceAll("`", "\\`")}\``;
      case "a": return `[${content()}](${element.dataset.mdHref || element.getAttribute("href") || ""})`;
      case "img": return `![${element.getAttribute("alt") || ""}](${element.dataset.mdSrc || ""})`;
      case "input": return "";
      default: return content();
    }
  }

  function listToMarkdown(list, depth = 0) {
    const ordered = list.tagName.toLowerCase() === "ol";
    const lines = [];
    [...list.children].filter((child) => child.tagName?.toLowerCase() === "li").forEach((item, index) => {
      const checkbox = item.querySelector(":scope > input[type='checkbox']");
      const inline = [...item.childNodes]
        .filter((child) => !(child.nodeType === Node.ELEMENT_NODE && ["ul", "ol", "input"].includes(child.tagName.toLowerCase())))
        .map(inlineToMarkdown).join("").trim();
      const marker = ordered ? `${index + 1}.` : "-";
      const task = checkbox ? `[${checkbox.checked ? "x" : " "}] ` : "";
      lines.push(`${"  ".repeat(depth)}${marker} ${task}${inline}`);
      [...item.children].filter((child) => ["ul", "ol"].includes(child.tagName.toLowerCase()))
        .forEach((nested) => lines.push(listToMarkdown(nested, depth + 1)));
    });
    return lines.join("\n");
  }

  function tableToMarkdown(table) {
    const rows = [...table.querySelectorAll("tr")];
    if (!rows.length) return "";
    const values = rows.map((row) => [...row.children].map((cell) => inlineToMarkdown(cell).replaceAll("|", "\\|").trim()));
    const width = Math.max(...values.map((row) => row.length));
    while (values[0].length < width) values[0].push("");
    const output = [`| ${values[0].join(" | ")} |`, `| ${Array(width).fill("---").join(" | ")} |`];
    values.slice(1).forEach((row) => {
      while (row.length < width) row.push("");
      output.push(`| ${row.join(" | ")} |`);
    });
    return output.join("\n");
  }

  function blockToMarkdown(element) {
    const tag = element.tagName.toLowerCase();
    if (element.classList.contains("protected-block") && element.dataset.raw) return element.dataset.raw;
    if (/^h[1-6]$/.test(tag)) return `${"#".repeat(Number(tag[1]))} ${inlineToMarkdown(element).trim()}`;
    if (tag === "p") return inlineToMarkdown(element).trim();
    if (tag === "div") return inlineToMarkdown(element).trim();
    if (tag === "hr") return "---";
    if (tag === "pre") {
      const language = element.dataset.language || "";
      return `\`\`\`${language}\n${element.textContent || ""}\n\`\`\``;
    }
    if (tag === "blockquote") {
      const body = [...element.children].map(blockToMarkdown).filter(Boolean).join("\n\n") || inlineToMarkdown(element);
      return body.split("\n").map((line) => `> ${line}`).join("\n");
    }
    if (tag === "ul" || tag === "ol") return listToMarkdown(element);
    if (tag === "table") return tableToMarkdown(element);
    if (tag === "br") return "";
    return inlineToMarkdown(element).trim();
  }

  function previewToMarkdown() {
    return [...previewEditor.children].map(blockToMarkdown).filter((block) => block !== "")
      .join("\n\n").replace(/\n{3,}/g, "\n\n");
  }

  function normalizePreviewDom() {
    const blockTags = new Set(["BLOCKQUOTE", "HR", "OL", "P", "PRE", "TABLE", "UL"]);
    previewEditor.querySelectorAll("li > li").forEach((nestedItem) => {
      const parentItem = nestedItem.parentElement;
      const parentList = parentItem?.parentElement;
      if (parentList && ["UL", "OL"].includes(parentList.tagName)) {
        parentList.insertBefore(nestedItem, parentItem.nextSibling);
      }
    });
    previewEditor.querySelectorAll("p, h1, h2, h3, h4, h5, h6").forEach((parent) => {
      if (![...parent.children].some((child) => blockTags.has(child.tagName))) return;
      const fragment = document.createDocumentFragment();
      let inline = parent.cloneNode(false);
      const flushInline = () => {
        if (!inline.childNodes.length) return;
        fragment.append(inline);
        inline = parent.cloneNode(false);
        inline.removeAttribute("data-source-start");
        inline.removeAttribute("data-source-end");
      };
      [...parent.childNodes].forEach((child) => {
        if (child.nodeType === Node.ELEMENT_NODE && blockTags.has(child.tagName)) {
          flushInline();
          fragment.append(child);
        } else {
          inline.append(child);
        }
      });
      flushInline();
      parent.replaceWith(fragment);
    });
  }

  function renderPreview() {
    state.applying = true;
    previewEditor.innerHTML = renderMarkdown(state.text);
    previewSelectionRange = null;
    state.previewChanged = false;
    state.applying = false;
  }

  function sourceViewportLine() {
    const lineCount = Math.max(1, sourceEditor.value.split("\n").length);
    const style = getComputedStyle(sourceEditor);
    const padding = (parseFloat(style.paddingTop) || 0) + (parseFloat(style.paddingBottom) || 0);
    const contentHeight = Math.max(1, sourceEditor.scrollHeight - padding);
    return Math.min(lineCount - 1, (sourceEditor.scrollTop / contentHeight) * lineCount);
  }

  function scrollPreviewToSourceLine(sourceLine) {
    const previewPane = $("#preview-pane");
    const blocks = [...previewEditor.querySelectorAll(":scope > [data-source-start]")];
    if (!blocks.length) {
      previewPane.scrollTop = 0;
      return;
    }

    let target = blocks[0];
    for (const block of blocks) {
      if (Number(block.dataset.sourceStart) > sourceLine) break;
      target = block;
    }

    const start = Number(target.dataset.sourceStart);
    const end = Math.max(start, Number(target.dataset.sourceEnd));
    const progress = end > start
      ? Math.min(1, Math.max(0, (sourceLine - start) / (end - start + 1)))
      : 0;
    const paneRect = previewPane.getBoundingClientRect();
    const targetRect = target.getBoundingClientRect();
    const targetTop = previewPane.scrollTop + targetRect.top - paneRect.top;
    previewPane.scrollTop = Math.max(0, targetTop + targetRect.height * progress - 16);
  }

  function previewViewportSourceLine() {
    const previewPane = $("#preview-pane");
    const blocks = [...previewEditor.querySelectorAll(":scope > [data-source-start]")];
    if (!blocks.length) return 0;
    const paneTop = previewPane.getBoundingClientRect().top + 16;
    let target = blocks[0];
    for (const block of blocks) {
      if (block.getBoundingClientRect().top > paneTop) break;
      target = block;
    }
    const bounds = target.getBoundingClientRect();
    const start = Number(target.dataset.sourceStart) || 0;
    const end = Math.max(start, Number(target.dataset.sourceEnd) || start);
    const progress = bounds.height > 0
      ? Math.min(1, Math.max(0, (paneTop - bounds.top) / bounds.height)) : 0;
    return start + (end - start + 1) * progress;
  }

  function scrollSourceToLine(sourceLine) {
    const lineCount = Math.max(1, sourceEditor.value.split("\n").length);
    const maximumScroll = Math.max(0, sourceEditor.scrollHeight - sourceEditor.clientHeight);
    sourceEditor.scrollTop = maximumScroll * Math.min(1, Math.max(0, sourceLine / lineCount));
    syncSourceHighlightScroll();
  }

  function scheduleSplitScroll(origin) {
    if (state.mode !== "split" || synchronizingSplitScroll) return;
    cancelAnimationFrame(splitScrollFrame);
    splitScrollFrame = requestAnimationFrame(() => {
      if (state.mode !== "split") return;
      synchronizingSplitScroll = true;
      if (origin === "source") scrollPreviewToSourceLine(sourceViewportLine());
      else scrollSourceToLine(previewViewportSourceLine());
      requestAnimationFrame(() => { synchronizingSplitScroll = false; });
    });
  }

  function markChanged() {
    if (state.applying) return;
    state.dirty = state.savedText === null || state.text !== state.savedText ||
      state.eol !== state.savedEol;
    updateChrome();
    post("document.changed", {
      text: state.text, mode: state.mode, eol: state.eol, dirty: state.dirty
    });
  }

  function setEol(eol) {
    const normalized = eol === "LF" ? "LF" : "CRLF";
    if (normalized === state.eol) return;
    rememberDocumentHistory();
    state.eol = normalized;
    markChanged();
  }

  function updatePosition() {
    if (activeEditorMode() !== "source") {
      $("#position-status").textContent = i18n.t("Rendered preview");
      return;
    }
    const before = sourceEditor.value.slice(0, sourceEditor.selectionStart);
    const lines = before.split("\n");
    $("#position-status").textContent = i18n.t("Line {line}, Column {column}", {
      line: lines.length, column: lines.at(-1).length + 1
    });
  }

  function updateChrome() {
    $("#document-name").textContent = state.path || state.origin === "googleDrive"
      ? state.name : i18n.t("Untitled");
    $("#dirty-indicator").hidden = !state.dirty;
    $("#save-status").textContent = i18n.t(state.googleDriveBusy
      ? "Working with Google Drive…" : state.dirty ? "Unsaved changes" : "Saved");
    $("#encoding-status").textContent = state.format === "mdz"
      ? `MDZ · ${state.encoding}` : state.encoding;
    $("#eol-status").textContent = state.eol;
    const modeToggle = $("#mode-toggle-button");
    const modeKeys = { source: "Source", split: "Split", preview: "Preview" };
    const modeIcons = { source: "</>", split: "◫", preview: "▣" };
    $("#mode-toggle-icon").textContent = modeIcons[state.mode];
    $("#mode-status").textContent = i18n.t(modeKeys[state.mode]);
    modeToggle.dataset.currentMode = state.mode;
    modeToggle.title = i18n.t("Editor mode");
    modeToggle.setAttribute("aria-label", modeToggle.title);
    $(".editor-stage").dataset.viewMode = state.mode;
    $(".editor-stage").dataset.activeEditor = activeEditorMode();
    $(".editor-stage").style.setProperty("--split-source-width", `${state.splitRatio}%`);
    $$('[data-mode-menu]').forEach((item) =>
      item.setAttribute("aria-checked", String(item.dataset.modeMenu === state.mode)));
    $$('[data-mode-menu="split"]').forEach((item) => {
      item.disabled = innerWidth < splitMinimumWidth && state.mode !== "split";
    });
    $$('[data-eol-menu]').forEach((item) =>
      item.setAttribute("aria-checked", String(item.dataset.eolMenu === state.eol)));
    $$('[data-menu-command="file.openGoogleDrive"], [data-menu-command="file.saveGoogleDriveAs"]')
      .forEach((item) => { item.disabled = state.googleDriveBusy; });
    updateHeadingChrome();
    updateFormattingChrome();
    updatePosition();
  }

  function setMode(mode, notifyNative = true) {
    if (!['source', 'split', 'preview'].includes(mode)) return;
    if (mode === "split" && innerWidth < splitMinimumWidth) mode = activeEditorMode();
    if (state.mode === mode) {
      $("#source-pane").hidden = mode === "preview";
      $("#preview-pane").hidden = mode === "source";
      $("#split-divider").hidden = mode !== "split";
      updateChrome();
      return;
    }
    const previousMode = state.mode;
    const previousEditor = activeEditorMode();
    const previewSourceLine = previousEditor === "source" && mode !== "source"
      ? sourceViewportLine()
      : null;
    if (previousEditor === "preview" && state.previewChanged) {
      state.text = previewToMarkdown();
      sourceEditor.value = state.text;
      updateSourceHighlight();
      state.previewChanged = false;
      markChanged();
    } else if (previousEditor === "source") {
      state.text = sourceEditor.value;
    }
    state.mode = mode;
    if (mode === "source") state.activeEditor = "source";
    else if (mode === "preview") state.activeEditor = "preview";
    else state.activeEditor = previousEditor;
    if (mode !== "source" && previousMode === "source") renderPreview();
    $("#source-pane").hidden = mode === "preview";
    $("#preview-pane").hidden = mode === "source";
    $("#split-divider").hidden = mode !== "split";
    updateChrome();
    (activeEditorMode() === "source" ? sourceEditor : previewEditor).focus();
    if (previewSourceLine !== null) {
      requestAnimationFrame(() => {
        if (state.mode !== "source") scrollPreviewToSourceLine(previewSourceLine);
      });
    }
    if (notifyNative) post("editor.modeChanged", { mode });
  }

  function applyDocument(documentState, resetEditor = true) {
    state.path = documentState.path || "";
    state.origin = documentState.origin === "googleDrive" ? "googleDrive" : "local";
    state.format = documentState.format === "mdz" ? "mdz" : "markdown";
    state.name = documentState.name || i18n.t("Untitled");
    state.text = documentState.text || "";
    state.dirty = Boolean(documentState.dirty);
    state.encoding = documentState.encoding || "UTF-8";
    state.eol = documentState.eol || "LF";
    state.savedText = state.dirty ? null : state.text;
    state.savedEol = state.eol;
    if (resetEditor) {
      state.applying = true;
      sourceEditor.value = state.text;
      resetDocumentHistory();
      updateSourceHighlight();
      renderPreview();
      state.applying = false;
    }
    updateChrome();
  }

  function findText() {
    const query = window.prompt(i18n.t("Text to find"));
    if (!query) return;
    if (activeEditorMode() === "preview") {
      window.find(query, false, false, true, false, false, false);
      return;
    }
    const haystack = sourceEditor.value.toLocaleLowerCase(i18n.locale);
    const needle = query.toLocaleLowerCase(i18n.locale);
    let index = haystack.indexOf(needle, sourceEditor.selectionEnd);
    if (index < 0) index = haystack.indexOf(needle);
    if (index >= 0) {
      sourceEditor.focus();
      sourceEditor.setSelectionRange(index, index + query.length);
      updatePosition();
    }
  }

  function editorCommand(name) {
    const editorMode = activeEditorMode();
    const target = editorMode === "source" ? sourceEditor : previewEditor;
    target.focus();
    if (name === "undo") {
      restoreDocumentHistory(documentUndoHistory, documentRedoHistory);
      return;
    }
    if (name === "redo") {
      restoreDocumentHistory(documentRedoHistory, documentUndoHistory);
      return;
    }
    if (name === "selectAll" && editorMode === "source") {
      sourceEditor.select();
      return;
    }
    if (name === "find") { findText(); return; }
    document.execCommand(name, false);
  }

  function documentHistorySnapshot() {
    const previewActive = activeEditorMode() === "preview";
    let previewHtml = null;
    if (previewActive) {
      const clone = previewEditor.cloneNode(true);
      const sourceCheckboxes = [...previewEditor.querySelectorAll('input[type="checkbox"]')];
      [...clone.querySelectorAll('input[type="checkbox"]')].forEach((checkbox, index) => {
        checkbox.toggleAttribute("checked", Boolean(sourceCheckboxes[index]?.checked));
      });
      previewHtml = clone.innerHTML;
    }
    return {
      text: previewActive ? previewToMarkdown() : sourceEditor.value,
      eol: state.eol,
      previewHtml,
      sourceStart: sourceEditor.selectionStart,
      sourceEnd: sourceEditor.selectionEnd
    };
  }

  function sameDocumentHistorySnapshot(left, right) {
    return Boolean(left && right) && left.text === right.text && left.eol === right.eol;
  }

  function pushDocumentHistory(history, snapshot) {
    if (sameDocumentHistorySnapshot(history.at(-1), snapshot)) return false;
    history.push(snapshot);
    if (history.length > maximumDocumentHistory) history.shift();
    return true;
  }

  function rememberDocumentHistory() {
    if (state.applying) return false;
    const recorded = pushDocumentHistory(documentUndoHistory, documentHistorySnapshot());
    if (recorded) documentRedoHistory.length = 0;
    return recorded;
  }

  function restoreDocumentHistory(from, to) {
    const current = documentHistorySnapshot();
    while (from.length && sameDocumentHistorySnapshot(from.at(-1), current)) from.pop();
    if (!from.length) {
      updateFormattingChrome();
      return false;
    }
    pushDocumentHistory(to, current);
    const snapshot = from.pop();
    state.applying = true;
    state.text = snapshot.text;
    state.eol = snapshot.eol;
    sourceEditor.value = snapshot.text;
    sourceEditor.setSelectionRange(
      Math.min(snapshot.sourceStart, snapshot.text.length),
      Math.min(snapshot.sourceEnd, snapshot.text.length));
    updateSourceHighlight();
    if (state.mode !== "source") {
      previewEditor.innerHTML = snapshot.previewHtml ?? renderMarkdown(snapshot.text);
      previewSelectionRange = null;
      state.previewChanged = false;
    }
    state.applying = false;
    markChanged();
    (activeEditorMode() === "preview" ? previewEditor : sourceEditor).focus();
    return true;
  }

  function resetDocumentHistory() {
    documentUndoHistory.length = 0;
    documentRedoHistory.length = 0;
    applyingSourceFormatting = false;
    applyingPreviewFormatting = false;
    sourceBeforeInputRecorded = false;
  }

  function rememberSourceFormatting() {
    const recorded = rememberDocumentHistory();
    applyingSourceFormatting = true;
    return recorded;
  }

  function replaceSourceRange(start, end, replacement, selectionStart = start,
                              selectionEnd = start + replacement.length) {
    rememberSourceFormatting();
    sourceEditor.focus();
    sourceEditor.setRangeText(replacement, start, end, "end");
    sourceEditor.setSelectionRange(selectionStart, selectionEnd);
    sourceEditor.dispatchEvent(new InputEvent("input", {
      bubbles: true,
      inputType: "insertText",
      data: replacement
    }));
    applyingSourceFormatting = false;
  }

  function wrapSource(prefix, suffix = prefix, placeholder = "text") {
    const start = sourceEditor.selectionStart;
    const end = sourceEditor.selectionEnd;
    const value = sourceEditor.value;
    const selected = value.slice(start, end);

    if (selected && selected.startsWith(prefix) && selected.endsWith(suffix) &&
        selected.length >= prefix.length + suffix.length) {
      const inner = selected.slice(prefix.length, selected.length - suffix.length);
      replaceSourceRange(start, end, inner, start, start + inner.length);
      return;
    }
    if (value.slice(Math.max(0, start - prefix.length), start) === prefix &&
        value.slice(end, end + suffix.length) === suffix) {
      replaceSourceRange(start - prefix.length, end + suffix.length, selected,
        start - prefix.length, end - prefix.length);
      return;
    }

    const content = selected || placeholder;
    const replacement = `${prefix}${content}${suffix}`;
    const contentStart = start + prefix.length;
    replaceSourceRange(start, end, replacement, contentStart, contentStart + content.length);
  }

  function sourceBlockRange() {
    const value = sourceEditor.value;
    const selectionStart = sourceEditor.selectionStart;
    const selectionEnd = sourceEditor.selectionEnd;
    const start = value.lastIndexOf("\n", selectionStart - 1) + 1;
    const last = selectionEnd > selectionStart && value[selectionEnd - 1] === "\n"
      ? selectionEnd - 1 : selectionEnd;
    const endIndex = value.indexOf("\n", last);
    return { start, end: endIndex < 0 ? value.length : endIndex };
  }

  function stripBlockPrefix(line) {
    const indentation = line.match(/^[ \t]*/)?.[0] || "";
    let content = line.slice(indentation.length);
    const quote = content.match(/^(?:>[ \t]?)+/)?.[0] || "";
    content = content.slice(quote.length);
    const withoutQuote = content;
    content = content.replace(/^(?:[-+*]|\d+\.)[ \t]+(?:\[[ xX]\][ \t]+)?/, "");
    return { indentation, quote, withoutQuote, content };
  }

  function applySourceBlockFormat(format) {
    const range = sourceBlockRange();
    const original = sourceEditor.value.slice(range.start, range.end);
    const lines = original.split("\n");
    const patterns = {
      bulletList: /^[ \t]*(?:>[ \t]?)*[-+*][ \t]+(?!\[[ xX]\])/,
      orderedList: /^[ \t]*(?:>[ \t]?)*\d+\.[ \t]+/,
      taskList: /^[ \t]*(?:>[ \t]?)*[-+*][ \t]+\[[ xX]\][ \t]+/,
      blockquote: /^[ \t]*>[ \t]?/
    };
    const active = patterns[format] && lines.every((line) => !line.trim() || patterns[format].test(line));
    let counter = 1;
    const replacement = lines.map((line) => {
      if (!line.trim() && lines.length > 1) return line;
      const { indentation, quote, withoutQuote, content: rawContent } = stripBlockPrefix(line);
      const content = rawContent || (lines.length === 1 ? i18n.t("List item") : "");
      if (format === "blockquote") {
        return active ? `${indentation}${withoutQuote}` : `${indentation}> ${withoutQuote || content}`;
      }
      if (active) return `${indentation}${quote}${rawContent}`;
      if (format === "bulletList") return `${indentation}${quote}- ${content}`;
      if (format === "orderedList") return `${indentation}${quote}${counter++}. ${content}`;
      if (format === "taskList") return `${indentation}${quote}- [ ] ${content}`;
      return line;
    }).join("\n");
    if (replacement !== original) {
      replaceSourceRange(range.start, range.end, replacement, range.start, range.start + replacement.length);
    }
  }

  function applySourceIndent(outdent = false) {
    const range = sourceBlockRange();
    const original = sourceEditor.value.slice(range.start, range.end);
    const replacement = original.split("\n").map((line) => {
      if (!outdent) return line ? `  ${line}` : line;
      if (line.startsWith("\t")) return line.slice(1);
      return line.replace(/^ {1,2}/, "");
    }).join("\n");
    if (replacement !== original) {
      replaceSourceRange(range.start, range.end, replacement, range.start, range.start + replacement.length);
    }
  }

  function insertSourceBlock(markdown) {
    const start = sourceEditor.selectionStart;
    const end = sourceEditor.selectionEnd;
    const before = sourceEditor.value.slice(0, start);
    const after = sourceEditor.value.slice(end);
    const prefix = before && !before.endsWith("\n\n") ? (before.endsWith("\n") ? "\n" : "\n\n") : "";
    const suffix = after && !after.startsWith("\n\n") ? (after.startsWith("\n") ? "\n" : "\n\n") : "";
    const replacement = `${prefix}${markdown}${suffix}`;
    const contentStart = start + prefix.length;
    replaceSourceRange(start, end, replacement, contentStart, contentStart + markdown.length);
  }

  function applySourceCodeBlock() {
    const range = sourceBlockRange();
    const selected = sourceEditor.value.slice(range.start, range.end);
    const fenced = selected.match(/^```([^\n]*)\n([\s\S]*?)\n```$/);
    if (fenced) {
      replaceSourceRange(range.start, range.end, fenced[2], range.start, range.start + fenced[2].length);
      return;
    }
    const content = selected || i18n.t("Code");
    const replacement = `\`\`\`\n${content}\n\`\`\``;
    replaceSourceRange(range.start, range.end, replacement,
      range.start + 4, range.start + 4 + content.length);
  }

  function clearSourceFormatting() {
    const start = sourceEditor.selectionStart;
    const end = sourceEditor.selectionEnd;
    if (start === end) return;
    const original = sourceEditor.value.slice(start, end);
    let replacement = original
      .replace(/!\[([^\]]*)\]\(([^)]+)\)/g, "$1")
      .replace(/\[([^\]]+)\]\(([^)]+)\)/g, "$1")
      .replace(/(\*\*|__|~~|`)([\s\S]*?)\1/g, "$2")
      .replace(/(^|[^*])\*([^*\n]+)\*/g, "$1$2")
      .replace(/(^|[^_])_([^_\n]+)_/g, "$1$2");
    if (replacement !== original) replaceSourceRange(start, end, replacement, start, start + replacement.length);
  }

  function rememberPreviewFormatting() {
    const recorded = rememberDocumentHistory();
    applyingPreviewFormatting = true;
    return recorded;
  }

  function commitPreviewChange() {
    normalizePreviewDom();
    const markdown = previewToMarkdown();
    state.previewChanged = state.mode !== "split";
    if (markdown !== state.text) {
      state.text = markdown;
      if (state.mode === "split") {
        sourceEditor.value = markdown;
        updateSourceHighlight();
      }
      markChanged();
    }
    rememberPreviewSelection();
    updateHeadingChrome();
    updateFormattingChrome();
  }

  function runPreviewFormatting(callback) {
    restorePreviewSelection();
    const before = previewEditor.innerHTML;
    const historyRecorded = rememberPreviewFormatting();
    callback();
    applyingPreviewFormatting = false;
    if (previewEditor.innerHTML === before) {
      if (historyRecorded) documentUndoHistory.pop();
      return false;
    }
    commitPreviewChange();
    return true;
  }

  function applySourceHeading(level) {
    const value = sourceEditor.value;
    const selectionStart = sourceEditor.selectionStart;
    const selectionEnd = sourceEditor.selectionEnd;
    const blockStart = value.lastIndexOf("\n", selectionStart - 1) + 1;
    const lastSelectedPosition = selectionEnd > selectionStart && value[selectionEnd - 1] === "\n"
      ? selectionEnd - 1 : selectionEnd;
    const blockEndIndex = value.indexOf("\n", lastSelectedPosition);
    const blockEnd = blockEndIndex < 0 ? value.length : blockEndIndex;
    const lines = value.slice(blockStart, blockEnd).split("\n");
    const replacement = lines.map((line) => {
      const heading = line.match(/^([ \t]{0,3})#{1,6}(?:[ \t]+|$)(.*)$/);
      const indentation = heading?.[1] ?? line.match(/^[ \t]{0,3}/)?.[0] ?? "";
      let content = heading ? heading[2] : line.slice(indentation.length);
      if (level === 0) return `${indentation}${content}`;
      if (!content && lines.length === 1) content = i18n.t("Heading");
      return `${indentation}${"#".repeat(level)} ${content}`;
    }).join("\n");
    if (replacement === value.slice(blockStart, blockEnd)) {
      updateChrome();
      return;
    }

    replaceSourceRange(blockStart, blockEnd, replacement, blockStart, blockStart + replacement.length);
  }

  function applyHeading(level) {
    if (!Number.isInteger(level) || level < 0 || level > 6) return;
    if (level > 0) {
      state.lastHeadingLevel = level;
      localStorage.setItem("mdviewer.lastHeadingLevel", String(level));
    }
    if (activeEditorMode() === "source") {
      applySourceHeading(level);
      return;
    }

    runPreviewFormatting(() =>
      document.execCommand("formatBlock", false, level === 0 ? "p" : `h${level}`));
  }

  function applyFormatting(format) {
    if (activeEditorMode() === "preview") {
      if (format === "image") { openImageDialog(); return; }
      if (format === "link") {
        const address = window.prompt(i18n.t("Link address"), "https://");
        if (!address) return;
        runPreviewFormatting(() => document.execCommand("createLink", false, address));
        return;
      }
      runPreviewFormatting(() => {
        if (format === "bold" || format === "italic") document.execCommand(format, false);
        else if (format === "strike") document.execCommand("strikeThrough", false);
        else if (format === "inlineCode") {
          const code = previewSelectionElement()?.closest?.("code");
          if (code && previewEditor.contains(code)) code.replaceWith(...code.childNodes);
          else {
            const selection = window.getSelection();
            const content = selection?.toString() || i18n.t("Code");
            document.execCommand("insertHTML", false, `<code>${escapeHtml(content)}</code>`);
          }
        } else if (format === "codeBlock") {
          const pre = previewSelectionElement()?.closest?.("pre");
          document.execCommand("formatBlock", false, pre ? "p" : "pre");
        } else if (format === "clear") {
          document.execCommand("removeFormat", false);
          document.execCommand("unlink", false);
        }
      });
      return;
    }
    sourceEditor.focus();
    if (format === "bold") wrapSource("**", "**");
    else if (format === "italic") wrapSource("*", "*");
    else if (format === "strike") wrapSource("~~", "~~");
    else if (format === "inlineCode") wrapSource("`", "`", i18n.t("Code"));
    else if (format === "codeBlock") applySourceCodeBlock();
    else if (format === "clear") clearSourceFormatting();
    else if (format === "image") openImageDialog();
    else if (format === "link") {
      const address = window.prompt(i18n.t("Link address"), "https://");
      if (address) wrapSource("[", `](${address})`, i18n.t("Link"));
    }
  }

  function applyBlockFormatting(format) {
    if (activeEditorMode() === "source") {
      if (["bulletList", "orderedList", "taskList", "blockquote"].includes(format)) {
        applySourceBlockFormat(format);
      } else if (format === "horizontalRule") insertSourceBlock("---");
      else if (format === "indent") applySourceIndent(false);
      else if (format === "outdent") applySourceIndent(true);
      return;
    }

    runPreviewFormatting(() => {
      if (format === "bulletList") document.execCommand("insertUnorderedList", false);
      else if (format === "orderedList") document.execCommand("insertOrderedList", false);
      else if (format === "taskList") {
        document.execCommand("insertUnorderedList", false);
        const list = previewSelectionElement()?.closest?.("ul");
        if (list && previewEditor.contains(list)) {
          const items = [...list.querySelectorAll(":scope > li")];
          const isTaskList = items.length && items.every((item) => item.dataset.task === "true");
          items.forEach((item) => {
            const checkbox = item.querySelector(":scope > input[type='checkbox']");
            if (isTaskList) {
              checkbox?.remove();
              delete item.dataset.task;
            } else if (!checkbox) {
              const input = document.createElement("input");
              input.type = "checkbox";
              input.contentEditable = "false";
              item.dataset.task = "true";
              item.prepend(input);
            }
          });
        }
      } else if (format === "blockquote") {
        const quote = previewSelectionElement()?.closest?.("blockquote");
        if (quote && previewEditor.contains(quote)) quote.replaceWith(...quote.childNodes);
        else document.execCommand("formatBlock", false, "blockquote");
      } else if (format === "horizontalRule") document.execCommand("insertHorizontalRule", false);
      else if (format === "indent" || format === "outdent") document.execCommand(format, false);
    });
  }

  function normalizeImageSource(value) {
    const source = String(value || "").trim().replaceAll("\\", "/");
    if (/^(?:https?:|data:)/i.test(source)) {
      return source.replaceAll(" ", "%20").replaceAll("(", "%28").replaceAll(")", "%29");
    }
    return source.split("/").map((part) => encodeURIComponent(part)).join("/");
  }

  function openImageDialog() {
    if (activeEditorMode() === "preview") {
      rememberPreviewSelection();
      pendingImageSourceSelection = null;
    } else {
      pendingImageSourceSelection = {
        start: sourceEditor.selectionStart,
        end: sourceEditor.selectionEnd
      };
    }
    pendingImageFileName = "";
    $("#image-dialog-form").reset();
    $("#image-dialog").showModal();
    $("#image-alt-input").focus();
  }

  function insertImage(source, alt = "") {
    const normalizedSource = normalizeImageSource(source);
    if (!normalizedSource) return;
    const safeAlt = String(alt || "").replaceAll("\\", "\\\\").replaceAll("]", "\\]").replaceAll("\n", " ");
    if (state.format === "mdz" &&
        /^data:image\/(?:png|jpeg|gif|webp|bmp);base64,/i.test(normalizedSource)) {
      if (!post("image.embed", {
        dataUrl: normalizedSource,
        fileName: pendingImageFileName || "image",
        alt: safeAlt
      })) {
        pendingImageSourceSelection = null;
        pendingImageFileName = "";
      }
      return;
    }
    if (activeEditorMode() === "source") {
      if (pendingImageSourceSelection) {
        sourceEditor.setSelectionRange(pendingImageSourceSelection.start, pendingImageSourceSelection.end);
      }
      wrapSource("![", `](${normalizedSource})`, safeAlt || i18n.t("Image description"));
    } else {
      runPreviewFormatting(() => {
        const sourceUrl = safeImageUrl(normalizedSource);
        if (!sourceUrl) return;
        document.execCommand("insertHTML", false,
          `<img src="${escapeHtml(sourceUrl)}" data-md-src="${escapeHtml(normalizedSource)}" alt="${escapeHtml(safeAlt)}">`);
      });
    }
    pendingImageSourceSelection = null;
    pendingImageFileName = "";
  }

  function fileToDataUrl(file) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.addEventListener("load", () => resolve(String(reader.result || "")), { once: true });
      reader.addEventListener("error", () => reject(reader.error || new Error("File read failed")), { once: true });
      reader.readAsDataURL(file);
    });
  }

  function createTableMarkdown(columns, rows) {
    const headers = Array.from({ length: columns }, (_, index) => `${i18n.t("Column")} ${index + 1}`);
    const body = Array.from({ length: rows }, () => `| ${Array(columns).fill("").join(" | ")} |`);
    return [`| ${headers.join(" | ")} |`, `| ${Array(columns).fill("---").join(" | ")} |`, ...body].join("\n");
  }

  function insertTable(columns, rows) {
    if (!Number.isInteger(columns) || !Number.isInteger(rows) || columns < 1 || rows < 1) return;
    if (activeEditorMode() === "source") {
      insertSourceBlock(createTableMarkdown(columns, rows));
      return;
    }
    const header = Array.from({ length: columns }, (_, index) =>
      `<th>${escapeHtml(i18n.t("Column"))} ${index + 1}</th>`).join("");
    const body = Array.from({ length: rows }, () =>
      `<tr>${Array.from({ length: columns }, () => "<td><br></td>").join("")}</tr>`).join("");
    runPreviewFormatting(() => document.execCommand("insertHTML", false,
      `<table><thead><tr>${header}</tr></thead><tbody>${body}</tbody></table><p><br></p>`));
  }

  function initializeTableGrid() {
    const grid = $("#table-size-grid");
    const cells = [];
    for (let row = 1; row <= 8; row += 1) {
      for (let column = 1; column <= 8; column += 1) {
        const button = document.createElement("button");
        button.type = "button";
        button.setAttribute("role", "gridcell");
        button.dataset.tableColumns = String(column);
        button.dataset.tableRows = String(row);
        button.setAttribute("aria-label", `${column} × ${row}`);
        cells.push(button);
      }
    }
    grid.replaceChildren(...cells);
    updateTableGridSelection(2, 2);
  }

  function updateTableGridSelection(columns, rows) {
    $("#table-size-label").textContent = `${columns} × ${rows}`;
    const grid = $("#table-size-grid");
    const cellSize = 18;
    const gap = 5;
    grid.style.setProperty("--table-selection-width", `${columns * cellSize + (columns - 1) * gap}px`);
    grid.style.setProperty("--table-selection-height", `${rows * cellSize + (rows - 1) * gap}px`);
    $$("#table-size-grid button").forEach((button) => {
      const selected = Number(button.dataset.tableColumns) <= columns &&
        Number(button.dataset.tableRows) <= rows;
      button.classList.toggle("is-selected", selected);
      button.setAttribute("aria-selected", String(selected));
    });
  }

  function sourceTableContext() {
    const value = sourceEditor.value;
    const lines = value.split("\n");
    const offsets = [];
    let offset = 0;
    lines.forEach((line) => { offsets.push(offset); offset += line.length + 1; });
    const caret = sourceEditor.selectionStart;
    let lineIndex = offsets.findLastIndex((lineOffset) => lineOffset <= caret);
    if (lineIndex < 0 || !lines[lineIndex]?.includes("|")) return null;
    let startLine = lineIndex;
    let endLine = lineIndex;
    while (startLine > 0 && lines[startLine - 1].includes("|") && lines[startLine - 1].trim()) startLine -= 1;
    while (endLine + 1 < lines.length && lines[endLine + 1].includes("|") && lines[endLine + 1].trim()) endLine += 1;
    if (startLine + 1 > endLine || !isTableDivider(lines[startLine + 1])) return null;

    const header = splitTableRow(lines[startLine]);
    const body = lines.slice(startLine + 2, endLine + 1).map(splitTableRow);
    const lineBeforeCaret = lines[lineIndex].slice(0, Math.max(0, caret - offsets[lineIndex]));
    let column = lineBeforeCaret.split("|").length - 1;
    if (lines[lineIndex].trimStart().startsWith("|")) column -= 1;
    column = Math.max(0, Math.min(header.length - 1, column));
    const dataRow = lineIndex >= startLine + 2 ? lineIndex - startLine - 1 : 0;
    return {
      start: offsets[startLine],
      end: offsets[endLine] + lines[endLine].length,
      header, body, column, dataRow
    };
  }

  function markdownFromTableParts(header, body) {
    const width = header.length;
    const normalizedBody = body.map((row) => Array.from({ length: width }, (_, index) => row[index] || ""));
    return [`| ${header.join(" | ")} |`, `| ${Array(width).fill("---").join(" | ")} |`,
      ...normalizedBody.map((row) => `| ${row.join(" | ")} |`)].join("\n");
  }

  function currentPreviewTableCell() {
    const cell = previewSelectionElement()?.closest?.("th, td");
    const table = cell?.closest?.("table");
    return table && previewEditor.contains(table) ? { table, cell } : null;
  }

  function placePreviewCaret(element) {
    const range = document.createRange();
    range.selectNodeContents(element);
    range.collapse(true);
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    previewSelectionRange = range.cloneRange();
  }

  function applySourceTableCommand(command, context) {
    const header = [...context.header];
    const body = context.body.map((row) => [...row]);
    let targetRow = context.dataRow;
    let targetColumn = context.column;
    if (command === "rowBefore" || command === "rowAfter") {
      const insertion = context.dataRow === 0 ? 0 : context.dataRow - 1 + (command === "rowAfter" ? 1 : 0);
      body.splice(insertion, 0, Array(header.length).fill(""));
      targetRow = insertion + 1;
    } else if (command === "rowDelete") {
      if (context.dataRow === 0 || !body.length) return;
      body.splice(context.dataRow - 1, 1);
      targetRow = Math.min(context.dataRow, body.length);
    } else if (command === "columnBefore" || command === "columnAfter") {
      const insertion = context.column + (command === "columnAfter" ? 1 : 0);
      header.splice(insertion, 0, `${i18n.t("Column")} ${header.length + 1}`);
      body.forEach((row) => row.splice(insertion, 0, ""));
      targetColumn = insertion;
    } else if (command === "columnDelete") {
      if (header.length <= 1) return;
      header.splice(context.column, 1);
      body.forEach((row) => row.splice(context.column, 1));
      targetColumn = Math.min(context.column, header.length - 1);
    }
    const markdown = markdownFromTableParts(header, body);
    replaceSourceRange(context.start, context.end, markdown, context.start, context.start + markdown.length);
    updateFormattingChrome({ dataRow: targetRow, column: targetColumn });
  }

  function applyPreviewTableCommand(command, context) {
    const clone = context.table.cloneNode(true);
    const allRows = [...clone.rows];
    const originalRows = [...context.table.rows];
    const rowIndex = originalRows.indexOf(context.cell.parentElement);
    const columnIndex = [...context.cell.parentElement.cells].indexOf(context.cell);
    const selectedIsHeader = context.cell.tagName.toLowerCase() === "th";
    let targetRowIndex = rowIndex;
    let targetColumnIndex = columnIndex;

    if (command === "rowBefore" || command === "rowAfter") {
      const body = clone.tBodies[0] || clone.createTBody();
      const bodyIndex = selectedIsHeader ? 0 : [...context.table.tBodies[0].rows].indexOf(context.cell.parentElement) +
        (command === "rowAfter" ? 1 : 0);
      const row = body.insertRow(bodyIndex);
      const width = allRows[0]?.cells.length || 1;
      for (let index = 0; index < width; index += 1) row.insertCell().append(document.createElement("br"));
      targetRowIndex = 1 + bodyIndex;
    } else if (command === "rowDelete") {
      if (selectedIsHeader) return;
      const bodyIndex = [...context.table.tBodies[0].rows].indexOf(context.cell.parentElement);
      clone.tBodies[0].deleteRow(bodyIndex);
      targetRowIndex = Math.min(rowIndex, clone.rows.length - 1);
    } else if (command === "columnBefore" || command === "columnAfter") {
      const insertion = columnIndex + (command === "columnAfter" ? 1 : 0);
      [...clone.rows].forEach((row, index) => {
        const cell = index === 0 ? document.createElement("th") : document.createElement("td");
        if (index === 0) cell.textContent = `${i18n.t("Column")} ${row.cells.length + 1}`;
        else cell.append(document.createElement("br"));
        row.insertBefore(cell, row.cells[insertion] || null);
      });
      targetColumnIndex = insertion;
    } else if (command === "columnDelete") {
      if ((allRows[0]?.cells.length || 0) <= 1) return;
      [...clone.rows].forEach((row) => row.deleteCell(columnIndex));
      targetColumnIndex = Math.min(columnIndex, clone.rows[0].cells.length - 1);
    }

    rememberPreviewFormatting();
    context.table.replaceWith(clone);
    applyingPreviewFormatting = false;
    const targetRow = clone.rows[Math.max(0, targetRowIndex)] || clone.rows[0];
    const targetCell = targetRow?.cells[Math.max(0, targetColumnIndex)] || targetRow?.cells[0];
    if (targetCell) placePreviewCaret(targetCell);
    commitPreviewChange();
  }

  function applyTableCommand(command) {
    if (activeEditorMode() === "source") {
      const context = sourceTableContext();
      if (context) applySourceTableCommand(command, context);
      return;
    }
    restorePreviewSelection();
    const context = currentPreviewTableCell();
    if (context) applyPreviewTableCommand(command, context);
  }

  function updateFormattingChrome() {
    const tableActions = $("#table-context-actions");
    let tableContext = null;
    previewEditor.querySelectorAll(".is-active-cell").forEach((cell) => {
      cell.classList.remove("is-active-cell");
      if (!cell.classList.length) cell.removeAttribute("class");
    });
    const editorMode = activeEditorMode();
    if (editorMode === "source") tableContext = sourceTableContext();
    else {
      tableContext = currentPreviewTableCell();
      tableContext?.cell.classList.add("is-active-cell");
    }
    tableActions.hidden = !tableContext;
    if (tableContext) {
      const rowDelete = tableActions.querySelector('[data-table-command="rowDelete"]');
      const columnDelete = tableActions.querySelector('[data-table-command="columnDelete"]');
      rowDelete.disabled = editorMode === "source"
        ? tableContext.dataRow === 0 || tableContext.body.length === 0
        : tableContext.cell.tagName.toLowerCase() === "th";
      const columnCount = editorMode === "source"
        ? tableContext.header.length : tableContext.table.rows[0]?.cells.length || 0;
      columnDelete.disabled = columnCount <= 1;
    }

    const previewStates = editorMode === "preview" ? {
      bold: document.queryCommandState("bold"), italic: document.queryCommandState("italic"),
      strike: document.queryCommandState("strikeThrough")
    } : {};
    $$('[data-format="bold"], [data-format="italic"], [data-format="strike"]').forEach((button) => {
      button.setAttribute("aria-pressed", String(Boolean(previewStates[button.dataset.format])));
    });
    const undoButton = $('[data-editor-command="undo"]');
    const redoButton = $('[data-editor-command="redo"]');
    undoButton.disabled = documentUndoHistory.length === 0;
    redoButton.disabled = documentRedoHistory.length === 0;
  }

  function createGoogleDriveMark() {
    const mark = document.createElement("span");
    mark.className = "google-drive-mark";
    mark.setAttribute("aria-hidden", "true");
    mark.innerHTML = '<svg viewBox="0 0 24 24" focusable="false">' +
      '<path fill="#0F9D58" d="M8.3 3h4.5l7.4 12.8h-4.6z"/>' +
      '<path fill="#4285F4" d="M6 15.8h14.2L18 19.7H3.8z"/>' +
      '<path fill="#F4B400" d="M8.3 3 3.8 10.8 6 14.7l6.8-11.7z"/></svg>';
    return mark;
  }

  function populateRecentDocuments(documents) {
    const menu = $("#recent-documents-menu");
    if (Array.isArray(documents)) recentDocumentItems = documents;
    const safeDocuments = recentDocumentItems.slice(0, 10).filter((item) =>
      item && ["local", "googleDrive"].includes(item.kind) &&
      typeof item.location === "string" && item.location &&
      typeof item.name === "string");
    if (!safeDocuments.length) {
      const empty = document.createElement("button");
      empty.type = "button";
      empty.disabled = true;
      empty.setAttribute("role", "menuitem");
      empty.textContent = i18n.t("No recent documents");
      menu.replaceChildren(empty);
      return;
    }
    menu.replaceChildren(...safeDocuments.map((recent) => {
      const button = document.createElement("button");
      button.type = "button";
      button.setAttribute("role", "menuitem");
      button.className = `recent-document-item ${recent.kind === "googleDrive" ? "google-drive" : "local"}`;
      button.dataset.recentKind = recent.kind;
      button.dataset.recentLocation = recent.location;
      button.title = recent.kind === "googleDrive"
        ? `${i18n.t("Google Drive")}: ${recent.name}` : recent.location;
      if (recent.kind === "googleDrive") button.append(createGoogleDriveMark());
      const name = document.createElement("span");
      name.className = "recent-document-name";
      name.textContent = recent.name;
      button.append(name);
      return button;
    }));
  }

  function populateLanguages() {
    const menu = $("#language-menu");
    menu.replaceChildren(...Object.entries(i18n.supportedLocales).map(([locale, descriptor]) => {
      const button = document.createElement("button");
      button.type = "button";
      button.setAttribute("role", "menuitemradio");
      button.setAttribute("aria-checked", String(locale === i18n.locale));
      button.dataset.locale = locale;
      button.textContent = descriptor.label;
      return button;
    }));
  }

  function closeMenus() {
    $$(".menu-trigger").forEach((trigger) => trigger.setAttribute("aria-expanded", "false"));
    $$(".menu-root > .menu-popup").forEach((popup) => { popup.hidden = true; });
    $$('[data-toolbar-picker] > button[aria-expanded]').forEach((trigger) =>
      trigger.setAttribute("aria-expanded", "false"));
    $$('[data-toolbar-picker] > .menu-popup').forEach((popup) => { popup.hidden = true; });
    $("#status-mode-menu").hidden = true;
    $("#mode-toggle-button").setAttribute("aria-expanded", "false");
  }

  function toggleStatusModeMenu() {
    const popup = $("#status-mode-menu");
    const opening = popup.hidden;
    closeMenus();
    if (opening) {
      popup.hidden = false;
      $("#mode-toggle-button").setAttribute("aria-expanded", "true");
    }
  }

  function toggleHeadingMenu() {
    const trigger = $("#heading-menu-button");
    const popup = $("#heading-menu-popup");
    const opening = popup.hidden;
    closeMenus();
    if (opening) {
      updateHeadingChrome();
      popup.hidden = false;
      trigger.setAttribute("aria-expanded", "true");
    }
  }

  function toggleToolbarPicker(root) {
    const trigger = root.querySelector(":scope > button");
    const popup = root.querySelector(":scope > .menu-popup");
    const opening = popup.hidden;
    closeMenus();
    if (opening) {
      popup.hidden = false;
      trigger.setAttribute("aria-expanded", "true");
      if (root.classList.contains("table-picker")) updateTableGridSelection(2, 2);
    }
  }

  function toggleMenu(root) {
    const trigger = root.querySelector(":scope > .menu-trigger");
    const popup = root.querySelector(":scope > .menu-popup");
    const opening = popup.hidden;
    closeMenus();
    if (opening) {
      if (root.matches("[data-file-menu-root]")) post("recent.refresh");
      popup.hidden = false;
      trigger.setAttribute("aria-expanded", "true");
    }
  }

  async function chooseLanguage(locale) {
    const applied = await i18n.setLocale(locale);
    populateLanguages();
    updateChrome();
    post("settings.languageChanged", { locale: applied });
  }

  function normalizeGoogleDriveFileName(value) {
    let name = String(value || "").trim();
    if (!name || name.length > 255 || /[\\/\r\n\0]/.test(name)) return "";
    if (!/\.[^.]+$/.test(name)) name += ".md";
    return /\.(?:md|markdown|mdz)$/i.test(name) ? name : "";
  }

  function openGoogleDriveSaveDialog() {
    if (state.googleDriveBusy) return;
    const dialog = $("#google-drive-save-dialog");
    const input = $("#google-drive-file-name");
    let suggested = state.name && state.name !== i18n.t("Untitled")
      ? state.name : `${i18n.t("Untitled")}.md`;
    if (!/\.(?:md|markdown|mdz)$/i.test(suggested)) {
      suggested += state.format === "mdz" ? ".mdz" : ".md";
    }
    input.value = suggested;
    input.setCustomValidity("");
    $("#google-drive-choose-folder").checked = true;
    dialog.showModal();
    requestAnimationFrame(() => {
      input.focus();
      const extension = suggested.lastIndexOf(".");
      input.setSelectionRange(0, extension > 0 ? extension : suggested.length);
    });
  }

  function executeMenuCommand(name) {
    if (name === "file.saveGoogleDriveAs") {
      openGoogleDriveSaveDialog();
    } else if (name.startsWith("file.") || name.startsWith("app.")) {
      post("command", { name });
    } else if (name.startsWith("edit.")) {
      editorCommand(name.slice(5));
    } else if (["view.source", "view.split", "view.preview"].includes(name)) {
      setMode(name.slice(5));
    } else if (name.startsWith("eol.")) {
      setEol(name.slice(4).toUpperCase());
    } else if (name === "view.fontSettings") {
      openFontSettings();
    } else if (name === "view.togglePreviewSpelling") {
      state.hidePreviewSpelling = !state.hidePreviewSpelling;
      applyEditorPreferences();
    } else if (name.startsWith("theme.")) {
      const theme = name.slice(6);
      applyTheme(theme);
      post("settings.themeChanged", { theme });
    }
    closeMenus();
  }

  function handleSourceSmartEnter(event) {
    if (event.key !== "Enter" || event.ctrlKey || event.altKey || event.metaKey ||
        sourceEditor.selectionStart !== sourceEditor.selectionEnd) return false;
    const caret = sourceEditor.selectionStart;
    const lineStart = sourceEditor.value.lastIndexOf("\n", caret - 1) + 1;
    const line = sourceEditor.value.slice(lineStart, caret);
    const match = line.match(/^(\s*)((?:>[ \t]?)*)(?:([-+*]|(\d+)\.)[ \t]+(\[[ xX]\][ \t]+)?)?(.*)$/);
    if (!match || (!match[2] && !match[3])) return false;
    const content = match[6] || "";
    const marker = match[3]
      ? (match[4] ? `${Number(match[4]) + 1}. ` : `${match[3]} `) + (match[5] ? "[ ] " : "")
      : "";
    const prefix = `${match[1]}${match[2]}${marker}`;
    event.preventDefault();
    if (!content.trim()) replaceSourceRange(lineStart, caret, "", lineStart, lineStart);
    else replaceSourceRange(caret, caret, `\n${prefix}`, caret + prefix.length + 1, caret + prefix.length + 1);
    return true;
  }

  function bindEvents() {
    const editorStage = $(".editor-stage");
    const containsExplorerFiles = (event) => {
      const transfer = event.dataTransfer;
      return Boolean(transfer &&
        (Array.from(transfer.types || []).includes("Files") || transfer.files?.length));
    };
    editorStage.addEventListener("dragenter", (event) => {
      if (!containsExplorerFiles(event)) return;
      event.preventDefault();
      editorStage.classList.add("is-file-drag-over");
    });
    editorStage.addEventListener("dragover", (event) => {
      if (!containsExplorerFiles(event)) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = "copy";
      editorStage.classList.add("is-file-drag-over");
    });
    editorStage.addEventListener("dragleave", (event) => {
      if (event.relatedTarget && editorStage.contains(event.relatedTarget)) return;
      editorStage.classList.remove("is-file-drag-over");
    });
    editorStage.addEventListener("drop", (event) => {
      if (!containsExplorerFiles(event)) return;
      event.preventDefault();
      event.stopPropagation();
      editorStage.classList.remove("is-file-drag-over");
      post("files.dropped");
    });
    window.addEventListener("blur", () =>
      editorStage.classList.remove("is-file-drag-over"));

    $("#mode-toggle-button").addEventListener("click", toggleStatusModeMenu);
    $("#status-mode-menu").addEventListener("click", (event) => {
      const item = event.target.closest("[data-status-mode]");
      if (!item || item.disabled) return;
      setMode(item.dataset.statusMode);
      closeMenus();
    });
    const splitDivider = $("#split-divider");
    const updateSplitRatio = (clientX) => {
      const bounds = $(".editor-stage").getBoundingClientRect();
      if (!bounds.width) return;
      state.splitRatio = Math.min(75, Math.max(25, ((clientX - bounds.left) / bounds.width) * 100));
      $(".editor-stage").style.setProperty("--split-source-width", `${state.splitRatio}%`);
    };
    splitDivider.addEventListener("pointerdown", (event) => {
      if (state.mode !== "split" || event.button !== 0) return;
      splitDivider.setPointerCapture(event.pointerId);
      splitDivider.classList.add("is-dragging");
      updateSplitRatio(event.clientX);
    });
    splitDivider.addEventListener("pointermove", (event) => {
      if (splitDivider.hasPointerCapture(event.pointerId)) updateSplitRatio(event.clientX);
    });
    const finishSplitDrag = (event) => {
      if (splitDivider.hasPointerCapture(event.pointerId)) splitDivider.releasePointerCapture(event.pointerId);
      splitDivider.classList.remove("is-dragging");
      localStorage.setItem("mdviewer.splitRatio", String(state.splitRatio));
    };
    splitDivider.addEventListener("pointerup", finishSplitDrag);
    splitDivider.addEventListener("pointercancel", finishSplitDrag);
    splitDivider.addEventListener("dblclick", () => {
      state.splitRatio = 50;
      localStorage.setItem("mdviewer.splitRatio", "50");
      updateChrome();
    });
    splitDivider.addEventListener("keydown", (event) => {
      if (!["ArrowLeft", "ArrowRight", "Home"].includes(event.key)) return;
      event.preventDefault();
      state.splitRatio = event.key === "Home" ? 50 :
        Math.min(75, Math.max(25, state.splitRatio + (event.key === "ArrowLeft" ? -2 : 2)));
      localStorage.setItem("mdviewer.splitRatio", String(state.splitRatio));
      updateChrome();
    });
    $("#theme-button").addEventListener("click", () => {
      const theme = state.theme === "dark" ? "light" : "dark";
      applyTheme(theme);
      post("settings.themeChanged", { theme });
    });
    $$(".format-actions button, .table-context-actions button").forEach((button) =>
      button.addEventListener("pointerdown", () => {
        if (activeEditorMode() === "preview") rememberPreviewSelection();
      }));
    $$("[data-format]").forEach((button) =>
      button.addEventListener("click", () => {
        applyFormatting(button.dataset.format);
        closeMenus();
      }));
    $$("[data-block-format]").forEach((button) =>
      button.addEventListener("click", () => {
        applyBlockFormatting(button.dataset.blockFormat);
        closeMenus();
      }));
    $$("[data-editor-command]").forEach((button) =>
      button.addEventListener("click", () => editorCommand(button.dataset.editorCommand)));
    $$("[data-table-command]").forEach((button) =>
      button.addEventListener("click", () => applyTableCommand(button.dataset.tableCommand)));
    $("#heading-menu-button").addEventListener("pointerdown", () => {
      if (activeEditorMode() === "preview") rememberPreviewSelection();
    });
    $("#heading-menu-button").addEventListener("click", toggleHeadingMenu);
    $$('[data-toolbar-picker]').filter((root) => !root.classList.contains("heading-picker"))
      .forEach((root) => root.querySelector(":scope > button")
        .addEventListener("click", () => toggleToolbarPicker(root)));
    $$("[data-heading-level]").forEach((item) => {
      item.addEventListener("pointerdown", () => {
        if (activeEditorMode() === "preview") rememberPreviewSelection();
      });
      item.addEventListener("click", () => {
        applyHeading(Number(item.dataset.headingLevel));
        closeMenus();
      });
    });

    const fontDialog = $("#font-settings-dialog");
    $("#font-settings-form").addEventListener("submit", () => applyFontSettings());
    $$('[data-dialog-cancel]').forEach((button) =>
      button.addEventListener("click", () => fontDialog.close("cancel")));
    fontDialog.addEventListener("click", (event) => {
      if (event.target === fontDialog) fontDialog.close("cancel");
    });

    const imageDialog = $("#image-dialog");
    $("#image-dialog-form").addEventListener("submit", (event) => {
      event.preventDefault();
      const source = $("#image-source-input").value;
      const alt = $("#image-alt-input").value;
      imageDialog.close("insert");
      requestAnimationFrame(() => insertImage(source, alt));
    });
    $$('[data-image-dialog-cancel]').forEach((button) =>
      button.addEventListener("click", () => {
        pendingImageSourceSelection = null;
        pendingImageFileName = "";
        imageDialog.close("cancel");
      }));
    imageDialog.addEventListener("click", (event) => {
      if (event.target === imageDialog) {
        pendingImageSourceSelection = null;
        pendingImageFileName = "";
        imageDialog.close("cancel");
      }
    });
    $("#image-file-button").addEventListener("click", () => $("#image-file-input").click());
    $("#image-file-input").addEventListener("change", async (event) => {
      const file = event.target.files?.[0];
      if (!file) return;
      try {
        pendingImageFileName = file.name || "image";
        $("#image-source-input").value = await fileToDataUrl(file);
        if (!$("#image-alt-input").value) {
          $("#image-alt-input").value = file.name.replace(/\.[^.]+$/, "");
        }
      } catch (error) {
        console.error(error);
      } finally {
        event.target.value = "";
      }
    });

    const googleDriveSaveDialog = $("#google-drive-save-dialog");
    const googleDriveFileName = $("#google-drive-file-name");
    googleDriveFileName.addEventListener("input", () =>
      googleDriveFileName.setCustomValidity(""));
    $("#google-drive-save-form").addEventListener("submit", (event) => {
      event.preventDefault();
      const fileName = normalizeGoogleDriveFileName(googleDriveFileName.value);
      if (!fileName) {
        googleDriveFileName.setCustomValidity(
          i18n.t("Use a .md or .markdown file name."));
        googleDriveFileName.reportValidity();
        return;
      }
      const chooseFolder = $("#google-drive-choose-folder").checked;
      googleDriveSaveDialog.close("save");
      post("command", { name: "file.saveGoogleDriveAs", fileName, chooseFolder });
    });
    $$('[data-google-drive-save-cancel]').forEach((button) =>
      button.addEventListener("click", () => googleDriveSaveDialog.close("cancel")));
    googleDriveSaveDialog.addEventListener("click", (event) => {
      if (event.target === googleDriveSaveDialog) googleDriveSaveDialog.close("cancel");
    });

    $("#table-size-grid").addEventListener("pointerover", (event) => {
      const cell = event.target.closest("button[data-table-columns]");
      if (cell) updateTableGridSelection(Number(cell.dataset.tableColumns), Number(cell.dataset.tableRows));
    });
    $("#table-size-grid").addEventListener("click", (event) => {
      const cell = event.target.closest("button[data-table-columns]");
      if (!cell) return;
      insertTable(Number(cell.dataset.tableColumns), Number(cell.dataset.tableRows));
      closeMenus();
    });

    $$(".menu-root").forEach((root) => {
      const trigger = root.querySelector(":scope > .menu-trigger");
      trigger.addEventListener("click", () => toggleMenu(root));
      trigger.addEventListener("pointerenter", () => {
        if ($$('.menu-trigger[aria-expanded="true"]').length) toggleMenu(root);
      });
    });
    $$("[data-menu-command]").forEach((item) =>
      item.addEventListener("click", () => executeMenuCommand(item.dataset.menuCommand)));
    $("#language-menu").addEventListener("click", (event) => {
      const item = event.target.closest("[data-locale]");
      if (!item) return;
      chooseLanguage(item.dataset.locale);
      closeMenus();
    });
    $("#recent-documents-menu").addEventListener("click", (event) => {
      const item = event.target.closest("[data-recent-kind][data-recent-location]");
      if (!item) return;
      post("recent.open", {
        kind: item.dataset.recentKind,
        location: item.dataset.recentLocation
      });
      closeMenus();
    });
    document.addEventListener("pointerdown", (event) => {
      if (!event.target.closest(".main-menu, [data-toolbar-picker], #mode-toggle-button, #status-mode-menu")) {
        closeMenus();
      }
    });

    $$("[data-window-command]").forEach((button) =>
      button.addEventListener("click", () =>
        post("command", { name: button.dataset.windowCommand })));
    const dragRegion = $("#titlebar-drag-region");
    dragRegion.addEventListener("mousedown", (event) => {
      if (event.button === 0 && event.detail === 1) {
        post("command", { name: "window.drag" });
      }
    });
    dragRegion.addEventListener("dblclick", () =>
      post("command", { name: "window.maximizeToggle" }));

    sourceEditor.addEventListener("beforeinput", (event) => {
      if (event.inputType === "historyUndo" || event.inputType === "historyRedo") {
        event.preventDefault();
        editorCommand(event.inputType === "historyUndo" ? "undo" : "redo");
        return;
      }
      if (state.applying || applyingSourceFormatting) return;
      rememberDocumentHistory();
      sourceBeforeInputRecorded = true;
    });
    sourceEditor.addEventListener("input", (event) => {
      if (!applyingSourceFormatting && !sourceBeforeInputRecorded && !event.isTrusted) {
        resetDocumentHistory();
      }
      sourceBeforeInputRecorded = false;
      state.text = sourceEditor.value;
      updateSourceHighlight();
      if (state.mode === "split") renderPreview();
      markChanged();
      updatePosition();
      updateHeadingChrome();
      updateFormattingChrome();
    });
    sourceEditor.addEventListener("focus", () => setActiveEditor("source"));
    ["click", "keyup", "select"].forEach((eventName) =>
      sourceEditor.addEventListener(eventName, () => {
        updatePosition();
        updateHeadingChrome();
        updateFormattingChrome();
      }));
    sourceEditor.addEventListener("scroll", () => {
      syncSourceHighlightScroll();
      scheduleSplitScroll("source");
    });
    sourceEditor.addEventListener("keydown", (event) => {
      if (handleSourceSmartEnter(event)) return;
      if (event.key === "Tab") {
        event.preventDefault();
        applySourceIndent(event.shiftKey);
      }
    });

    previewEditor.addEventListener("beforeinput", (event) => {
      if (event.inputType === "historyUndo" || event.inputType === "historyRedo") {
        event.preventDefault();
        editorCommand(event.inputType === "historyUndo" ? "undo" : "redo");
        return;
      }
      if (!state.applying && !applyingPreviewFormatting) rememberDocumentHistory();
    });
    previewEditor.addEventListener("input", () => {
      commitPreviewChange();
    });
    previewEditor.addEventListener("focus", () => setActiveEditor("preview"));
    ["mouseup", "keyup"].forEach((eventName) =>
      previewEditor.addEventListener(eventName, () => {
        rememberPreviewSelection();
        updateHeadingChrome();
        updateFormattingChrome();
      }));
    previewEditor.addEventListener("pointerdown", (event) => {
      if (event.target.matches('input[type="checkbox"]')) rememberPreviewFormatting();
    });
    previewEditor.addEventListener("keydown", (event) => {
      if (event.key === " " && event.target.matches('input[type="checkbox"]')) {
        rememberPreviewFormatting();
      }
    });
    previewEditor.addEventListener("change", (event) => {
      if (!event.target.matches('input[type="checkbox"]')) return;
      applyingPreviewFormatting = false;
      commitPreviewChange();
    });
    previewEditor.addEventListener("click", (event) => {
      const anchor = event.target.closest("a[href]");
      if (!anchor) return;
      event.preventDefault();
      post("openExternal", { url: anchor.dataset.mdHref || anchor.href });
    });
    $("#preview-pane").addEventListener("scroll", () => scheduleSplitScroll("preview"));

    [sourceEditor, previewEditor].forEach((editor) => {
      editor.addEventListener("paste", async (event) => {
        const item = [...(event.clipboardData?.items || [])].find((candidate) =>
          candidate.kind === "file" && candidate.type.startsWith("image/"));
        const file = item?.getAsFile();
        if (!file) return;
        event.preventDefault();
        const pasteMode = activeEditorMode();
        const sourceSelection = pasteMode === "source"
          ? { start: sourceEditor.selectionStart, end: sourceEditor.selectionEnd } : null;
        if (pasteMode === "preview") rememberPreviewSelection();
        try {
          const dataUrl = await fileToDataUrl(file);
          if (activeEditorMode() !== pasteMode) return;
          pendingImageSourceSelection = sourceSelection;
          pendingImageFileName = file.name || "image";
          insertImage(dataUrl, file.name ? file.name.replace(/\.[^.]+$/, "") : "");
        } catch (error) {
          console.error(error);
        }
      });
    });

    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") closeMenus();
      if (event.target.closest?.("dialog")) return;
      if (event.key === "Tab" && activeEditorMode() === "preview" && previewEditor.contains(event.target)) {
        event.preventDefault();
        applyBlockFormatting(event.shiftKey ? "outdent" : "indent");
        return;
      }
      if (!event.ctrlKey || event.altKey) return;
      const key = event.key.toLowerCase();
      if (key === "z" && !event.shiftKey) {
        event.preventDefault();
        editorCommand("undo");
      } else if ((key === "y" && !event.shiftKey) || (key === "z" && event.shiftKey)) {
        event.preventDefault();
        editorCommand("redo");
      } else if (!event.shiftKey && /^[0-6]$/.test(key) && !$("#font-settings-dialog").open) {
        event.preventDefault();
        applyHeading(Number(key));
      } else if (key === "b" && !event.shiftKey) {
        event.preventDefault(); applyFormatting("bold");
      } else if (key === "i" && !event.shiftKey) {
        event.preventDefault(); applyFormatting("italic");
      } else if (key === "x" && event.shiftKey) {
        event.preventDefault(); applyFormatting("strike");
      } else if (key === "e" && !event.shiftKey) {
        event.preventDefault(); applyFormatting("inlineCode");
      } else if (key === "s") {
        event.preventDefault();
        post("command", { name: event.shiftKey ? "file.saveAs" : "file.save" });
      } else if (key === "o") {
        event.preventDefault(); post("command", { name: "file.open" });
      } else if (key === "n") {
        event.preventDefault(); post("command", { name: "file.new" });
      } else if (key === "m" && event.shiftKey) {
        event.preventDefault();
        const modes = innerWidth >= splitMinimumWidth
          ? ["source", "split", "preview"] : ["source", "preview"];
        setMode(modes[(modes.indexOf(state.mode) + 1) % modes.length]);
      } else if (key === "f") {
        event.preventDefault(); findText();
      }
    });

    window.addEventListener("mdviewerhostmessage", async (event) => {
      const message = event.detail || {};
      if (message.type === "app.init" || message.type === "document.opened") {
        applyTheme(message.theme || state.theme);
        await i18n.setLocale(message.language || i18n.locale);
        populateLanguages();
        applyDocument(message.document || {});
        setMode(message.mode || "preview", false);
      } else if (message.type === "document.saved") {
        state.dirty = false;
        state.path = message.document?.path || state.path;
        state.origin = message.document?.origin || state.origin;
        state.format = message.document?.format === "mdz" ? "mdz" : "markdown";
        state.name = message.document?.name || state.name;
        state.encoding = message.document?.encoding || state.encoding;
        state.eol = message.document?.eol || state.eol;
        state.savedText = state.text;
        state.savedEol = state.eol;
        updateChrome();
        if (message.document?.origin === "googleDrive") {
          showToast(i18n.t("Saved to Google Drive: {name}", { name: state.name }), "success");
        }
      } else if (message.type === "document.savedSnapshot") {
        const documentState = message.document || {};
        state.path = documentState.path || "";
        state.origin = documentState.origin === "googleDrive" ? "googleDrive" : "local";
        state.format = documentState.format === "mdz" ? "mdz" : "markdown";
        state.name = documentState.name || state.name;
        state.encoding = documentState.encoding || state.encoding;
        state.eol = documentState.eol || state.eol;
        state.savedText = typeof message.savedText === "string"
          ? message.savedText : state.savedText;
        state.savedEol = message.savedEol === "LF" ? "LF" : "CRLF";
        state.dirty = state.text !== state.savedText || state.eol !== state.savedEol;
        updateChrome();
      } else if (message.type === "recent.changed") {
        populateRecentDocuments(message.documents);
      } else if (message.type === "googleDrive.busy") {
        state.googleDriveBusy = Boolean(message.busy);
        updateChrome();
      } else if (message.type === "googleDrive.savedOlderRevision") {
        $("#save-status").textContent = i18n.t("Drive saved; newer local changes remain");
        showToast(i18n.t("Drive saved; newer local changes remain"), "warning");
      } else if (message.type === "native.toast") {
        showToast(message.message || "", message.tone || "info", message.title || "");
      } else if (message.type === "image.embedded") {
        insertImage(message.path || "", message.alt || "");
      } else if (message.type === "editor.setMode") {
        setMode(message.mode, false);
      } else if (message.type === "editor.command") {
        editorCommand(message.name);
      } else if (message.type === "language.changed") {
        await i18n.setLocale(message.locale);
        populateLanguages();
        updateChrome();
      } else if (message.type === "theme.changed") {
        applyTheme(message.theme);
      } else if (message.type === "window.stateChanged") {
        const maximized = Boolean(message.maximized);
        const button = $("#maximize-button");
        button.textContent = maximized ? "❐" : "□";
        const labelKey = maximized ? "Restore" : "Maximize";
        button.dataset.i18nTitle = labelKey;
        button.dataset.i18nAriaLabel = labelKey;
        button.title = i18n.t(labelKey);
        button.setAttribute("aria-label", button.title);
      }
    });

    window.addEventListener("mdviewerlanguageapplied", () => {
      updateChrome();
      populateRecentDocuments();
    });
    window.addEventListener("resize", () => {
      if (state.mode === "split" && innerWidth < splitMinimumWidth) setMode(activeEditorMode());
      else updateChrome();
    });
  }

  async function initialize() {
    await i18n.initialize();
    populateLanguages();
    initializeTableGrid();
    bindEvents();
    applyTheme(state.theme);
    applyEditorPreferences();
    updateSourceHighlight();
    updateChrome();
    post("ready", { language: i18n.locale, theme: state.theme });
  }

  initialize().catch((error) => {
    console.error(error);
    document.body.textContent = `MdViewer UI startup failed: ${error.message}`;
  });
})();
