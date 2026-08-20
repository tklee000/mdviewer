(() => {
  "use strict";

  const i18n = window.MdViewerI18n;
  const requestedTheme = new URLSearchParams(location.search).get("theme") ||
    localStorage.getItem("mdviewer.theme") || "dark";
  document.documentElement.dataset.theme = requestedTheme === "light" ? "light" : "dark";
  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => [...document.querySelectorAll(selector)];
  const sourceEditor = $("#source-editor");
  const previewEditor = $("#preview-editor");

  const state = {
    text: "",
    mode: "source",
    path: "",
    name: "Untitled",
    dirty: false,
    encoding: "UTF-8",
    eol: "CRLF",
    theme: document.documentElement.dataset.theme,
    previewChanged: false,
    applying: false
  };

  function post(type, payload = {}) {
    const bridge = window.mdViewerNative;
    if (!bridge?.postMessage) return false;
    bridge.postMessage(JSON.stringify({ type, ...payload }));
    return true;
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

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (character) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;"
    })[character]);
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
    if (/^[a-z][a-z0-9+.-]*:/i.test(url) || url.startsWith("//") || url.startsWith("/")) return "";
    if (!url) return "";
    return `https://app.mdviewer/__asset?path=${encodeURIComponent(url.replaceAll("\\", "/"))}`;
  }

  function renderInline(value) {
    const tokens = [];
    const hold = (html) => `\u0000${tokens.push(html) - 1}\u0000`;
    let text = String(value || "");

    text = text.replace(/`([^`\n]+)`/g, (_, code) =>
      hold(`<code>${escapeHtml(code)}</code>`));
    text = text.replace(/!\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g,
      (_, alt, rawUrl) => {
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

  function renderMarkdown(markdown) {
    const lines = String(markdown || "").replaceAll("\r\n", "\n").split("\n");
    const output = [];
    let index = 0;

    if (lines[0]?.trim() === "---") {
      const end = lines.slice(1).findIndex((line) => line.trim() === "---");
      if (end >= 0) {
        const last = end + 1;
        const raw = lines.slice(0, last + 1).join("\n");
        output.push(`<div class="protected-block" contenteditable="false" data-raw="${escapeHtml(raw)}">${escapeHtml(raw)}</div>`);
        index = last + 1;
      }
    }

    while (index < lines.length) {
      const line = lines[index];
      if (!line.trim()) { index += 1; continue; }

      const fence = line.match(/^ {0,3}(```|~~~)\s*([^\s]*)\s*$/);
      if (fence) {
        const marker = fence[1];
        const language = fence[2] || "";
        const code = [];
        index += 1;
        while (index < lines.length && !new RegExp(`^ {0,3}${marker}`).test(lines[index])) {
          code.push(lines[index]);
          index += 1;
        }
        if (index < lines.length) index += 1;
        output.push(`<pre data-language="${escapeHtml(language)}"><code>${escapeHtml(code.join("\n"))}</code></pre>`);
        continue;
      }

      const heading = line.match(/^ {0,3}(#{1,6})\s+(.+?)\s*#*$/);
      if (heading) {
        const level = heading[1].length;
        output.push(`<h${level}>${renderInline(heading[2])}</h${level}>`);
        index += 1;
        continue;
      }

      if (/^\s*((\*\s*){3,}|(-\s*){3,}|(_\s*){3,})$/.test(line)) {
        output.push("<hr>");
        index += 1;
        continue;
      }

      if (index + 1 < lines.length && line.includes("|") && isTableDivider(lines[index + 1])) {
        const headers = splitTableRow(line);
        const rows = [];
        index += 2;
        while (index < lines.length && lines[index].includes("|") && lines[index].trim()) {
          rows.push(splitTableRow(lines[index]));
          index += 1;
        }
        output.push(`<table><thead><tr>${headers.map((cell) => `<th>${renderInline(cell)}</th>`).join("")}</tr></thead>` +
          `<tbody>${rows.map((row) => `<tr>${headers.map((_, column) => `<td>${renderInline(row[column] || "")}</td>`).join("")}</tr>`).join("")}</tbody></table>`);
        continue;
      }

      if (/^\s*>/.test(line)) {
        const quoted = [];
        while (index < lines.length && /^\s*>/.test(lines[index])) {
          quoted.push(lines[index].replace(/^\s*>\s?/, ""));
          index += 1;
        }
        output.push(`<blockquote>${renderMarkdown(quoted.join("\n"))}</blockquote>`);
        continue;
      }

      const listMatch = line.match(/^\s*([-+*]|\d+\.)\s+(.+)$/);
      if (listMatch) {
        const ordered = /\d+\./.test(listMatch[1]);
        const tag = ordered ? "ol" : "ul";
        const items = [];
        while (index < lines.length) {
          const item = lines[index].match(/^\s*([-+*]|\d+\.)\s+(.+)$/);
          if (!item || /\d+\./.test(item[1]) !== ordered) break;
          const task = item[2].match(/^\[([ xX])\]\s*(.*)$/);
          items.push(task
            ? `<li data-task="true"><input type="checkbox" contenteditable="false" disabled${task[1].toLowerCase() === "x" ? " checked" : ""}>${renderInline(task[2])}</li>`
            : `<li>${renderInline(item[2])}</li>`);
          index += 1;
        }
        output.push(`<${tag}>${items.join("")}</${tag}>`);
        continue;
      }

      const paragraph = [line.trim()];
      index += 1;
      while (index < lines.length && lines[index].trim() && !isBlockStart(lines, index)) {
        paragraph.push(lines[index].trim());
        index += 1;
      }
      output.push(`<p>${renderInline(paragraph.join("\n"))}</p>`);
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
      case "del": case "s": return `~~${content()}~~`;
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

  function renderPreview() {
    state.applying = true;
    previewEditor.innerHTML = renderMarkdown(state.text);
    state.previewChanged = false;
    state.applying = false;
  }

  function markChanged() {
    if (state.applying) return;
    state.dirty = true;
    updateChrome();
    post("document.changed", { text: state.text, mode: state.mode });
  }

  function updatePosition() {
    if (state.mode !== "source") {
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
    $("#document-name").textContent = state.path ? state.name : i18n.t("Untitled");
    $("#dirty-indicator").hidden = !state.dirty;
    $("#save-status").textContent = i18n.t(state.dirty ? "Unsaved changes" : "Saved");
    $("#encoding-status").textContent = state.encoding;
    $("#eol-status").textContent = state.eol;
    $("#mode-status").textContent = i18n.t(state.mode === "source" ? "Source editing" : "Preview editing");
    $("#source-mode-button").setAttribute("aria-pressed", String(state.mode === "source"));
    $("#preview-mode-button").setAttribute("aria-pressed", String(state.mode === "preview"));
    $$('[data-mode-menu]').forEach((item) =>
      item.setAttribute("aria-checked", String(item.dataset.modeMenu === state.mode)));
    updatePosition();
  }

  function setMode(mode, notifyNative = true) {
    if (!['source', 'preview'].includes(mode)) return;
    if (state.mode === mode) {
      $("#source-pane").hidden = mode !== "source";
      $("#preview-pane").hidden = mode !== "preview";
      updateChrome();
      return;
    }
    if (state.mode === "preview" && mode !== "preview" && state.previewChanged) {
      state.text = previewToMarkdown();
      sourceEditor.value = state.text;
      markChanged();
    } else if (state.mode === "source") {
      state.text = sourceEditor.value;
    }
    state.mode = mode;
    if (mode === "preview") renderPreview();
    $("#source-pane").hidden = mode !== "source";
    $("#preview-pane").hidden = mode !== "preview";
    updateChrome();
    (mode === "source" ? sourceEditor : previewEditor).focus();
    if (notifyNative) post("editor.modeChanged", { mode });
  }

  function applyDocument(documentState, resetEditor = true) {
    state.path = documentState.path || "";
    state.name = documentState.name || i18n.t("Untitled");
    state.text = documentState.text || "";
    state.dirty = Boolean(documentState.dirty);
    state.encoding = documentState.encoding || "UTF-8";
    state.eol = documentState.eol || "LF";
    if (resetEditor) {
      state.applying = true;
      sourceEditor.value = state.text;
      renderPreview();
      state.applying = false;
    }
    updateChrome();
  }

  function findText() {
    const query = window.prompt(i18n.t("Text to find"));
    if (!query) return;
    if (state.mode === "preview") {
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
    const target = state.mode === "source" ? sourceEditor : previewEditor;
    target.focus();
    if (name === "selectAll" && state.mode === "source") {
      sourceEditor.select();
      return;
    }
    if (name === "find") { findText(); return; }
    document.execCommand(name, false);
  }

  function wrapSource(prefix, suffix = prefix, placeholder = "text") {
    const start = sourceEditor.selectionStart;
    const end = sourceEditor.selectionEnd;
    const selected = sourceEditor.value.slice(start, end) || placeholder;
    sourceEditor.setRangeText(`${prefix}${selected}${suffix}`, start, end, "select");
    sourceEditor.dispatchEvent(new Event("input", { bubbles: true }));
  }

  function applyFormatting(format) {
    if (state.mode === "preview") {
      previewEditor.focus();
      if (format === "bold" || format === "italic") document.execCommand(format, false);
      else if (format === "heading") document.execCommand("formatBlock", false, "h2");
      else if (format === "code") document.execCommand("formatBlock", false, "pre");
      else if (format === "link") {
        const address = window.prompt(i18n.t("Link address"), "https://");
        if (address) document.execCommand("createLink", false, address);
      }
      state.previewChanged = true;
      state.text = previewToMarkdown();
      markChanged();
      return;
    }
    sourceEditor.focus();
    if (format === "bold") wrapSource("**", "**");
    else if (format === "italic") wrapSource("*", "*");
    else if (format === "heading") wrapSource("## ", "", i18n.t("Heading"));
    else if (format === "code") wrapSource("`", "`");
    else if (format === "link") {
      const address = window.prompt(i18n.t("Link address"), "https://");
      if (address) wrapSource("[", `](${address})`, i18n.t("Link"));
    }
  }

  function populateLanguages() {
    const select = $("#language-select");
    select.replaceChildren(...Object.entries(i18n.supportedLocales).map(([locale, descriptor]) => {
      const option = document.createElement("option");
      option.value = locale;
      option.textContent = descriptor.label;
      return option;
    }));
    select.value = i18n.locale;

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
  }

  function toggleMenu(root) {
    const trigger = root.querySelector(":scope > .menu-trigger");
    const popup = root.querySelector(":scope > .menu-popup");
    const opening = popup.hidden;
    closeMenus();
    if (opening) {
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

  function executeMenuCommand(name) {
    if (name.startsWith("file.") || name.startsWith("app.")) {
      post("command", { name });
    } else if (name.startsWith("edit.")) {
      editorCommand(name.slice(5));
    } else if (name === "view.source") {
      setMode("source");
    } else if (name === "view.preview") {
      setMode("preview");
    } else if (name.startsWith("theme.")) {
      const theme = name.slice(6);
      applyTheme(theme);
      post("settings.themeChanged", { theme });
    }
    closeMenus();
  }

  function bindEvents() {
    $("#source-mode-button").addEventListener("click", () => setMode("source"));
    $("#preview-mode-button").addEventListener("click", () => setMode("preview"));
    $("#theme-button").addEventListener("click", () => {
      const theme = state.theme === "dark" ? "light" : "dark";
      applyTheme(theme);
      post("settings.themeChanged", { theme });
    });
    $$("[data-format]").forEach((button) =>
      button.addEventListener("click", () => applyFormatting(button.dataset.format)));

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
    document.addEventListener("pointerdown", (event) => {
      if (!event.target.closest(".main-menu")) closeMenus();
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

    sourceEditor.addEventListener("input", () => {
      state.text = sourceEditor.value;
      markChanged();
      updatePosition();
    });
    ["click", "keyup", "select"].forEach((eventName) =>
      sourceEditor.addEventListener(eventName, updatePosition));

    previewEditor.addEventListener("input", () => {
      state.previewChanged = true;
      state.text = previewToMarkdown();
      markChanged();
    });
    previewEditor.addEventListener("click", (event) => {
      const anchor = event.target.closest("a[href]");
      if (!anchor) return;
      event.preventDefault();
      post("openExternal", { url: anchor.dataset.mdHref || anchor.href });
    });

    $("#language-select").addEventListener("change", (event) =>
      chooseLanguage(event.target.value));

    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") closeMenus();
      if (!event.ctrlKey || event.altKey) return;
      const key = event.key.toLowerCase();
      if (key === "s") {
        event.preventDefault();
        post("command", { name: event.shiftKey ? "file.saveAs" : "file.save" });
      } else if (key === "o") {
        event.preventDefault(); post("command", { name: "file.open" });
      } else if (key === "n") {
        event.preventDefault(); post("command", { name: "file.new" });
      } else if (key === "m" && event.shiftKey) {
        event.preventDefault(); setMode(state.mode === "source" ? "preview" : "source");
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
        setMode(message.mode || "source", false);
      } else if (message.type === "document.saved") {
        state.dirty = false;
        state.path = message.document?.path || state.path;
        state.name = message.document?.name || state.name;
        state.encoding = message.document?.encoding || state.encoding;
        state.eol = message.document?.eol || state.eol;
        updateChrome();
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

    window.addEventListener("mdviewerlanguageapplied", updateChrome);
  }

  async function initialize() {
    await i18n.initialize();
    populateLanguages();
    bindEvents();
    applyTheme(state.theme);
    updateChrome();
    post("ready", { language: i18n.locale, theme: state.theme });
  }

  initialize().catch((error) => {
    console.error(error);
    document.body.textContent = `MdViewer UI startup failed: ${error.message}`;
  });
})();
