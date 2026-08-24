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
    googleDriveAvailable: true,
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
  let pdfPreviewTimer = 0;
  let pdfPreviewSequence = 0;
  let activePdfPreviewRequest = 0;
  let pdfDialogMode = "export";
  let pdfPreviewFrameLoaded = false;
  let pdfPreviewState = "loading";
  let pdfPrintersReady = false;
  let pdfPrintBusy = false;
  let pdfPrinterPropertiesBusy = false;
  let advancedSettingsPrinterName = "";
  let preferredPrinterName = "";
  let docxExportBusy = false;
  let hwpxExportBusy = false;
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

  function readStoredPdfSettings() {
    const defaults = {
      paper: "a4", orientation: "portrait", marginMm: 20,
      pageNumbers: false, printBackground: true
    };
    try {
      const stored = JSON.parse(localStorage.getItem("mdviewer.pdfSettings") || "null");
      if (!stored || typeof stored !== "object") return defaults;
      return {
        paper: ["a4", "letter"].includes(stored.paper) ? stored.paper : defaults.paper,
        orientation: ["portrait", "landscape"].includes(stored.orientation)
          ? stored.orientation : defaults.orientation,
        marginMm: [0, 10, 20].includes(Number(stored.marginMm))
          ? Number(stored.marginMm) : defaults.marginMm,
        pageNumbers: Boolean(stored.pageNumbers),
        printBackground: stored.printBackground !== false
      };
    } catch {
      return defaults;
    }
  }

  function readStoredPrintSettings() {
    const defaults = {
      pageMode: "all", pageRange: "", printerName: "", copies: 1
    };
    try {
      const stored = JSON.parse(localStorage.getItem("mdviewer.printSettings") || "null");
      if (!stored || typeof stored !== "object") return defaults;
      return {
        pageMode: stored.pageMode === "custom" ? "custom" : "all",
        pageRange: typeof stored.pageRange === "string"
          ? stored.pageRange.slice(0, 128) : "",
        printerName: typeof stored.printerName === "string"
          ? stored.printerName.slice(0, 1024) : "",
        copies: Number.isInteger(Number(stored.copies)) &&
          Number(stored.copies) >= 1 && Number(stored.copies) <= 999
          ? Number(stored.copies) : 1
      };
    } catch {
      return defaults;
    }
  }

  function normalizePdfPageRange(value) {
    const text = String(value || "").trim();
    if (!text || text.length > 128) return null;
    const normalized = [];
    for (const rawPart of text.split(",")) {
      const part = rawPart.trim();
      const match = part.match(/^(\d+)(?:\s*-\s*(\d+))?$/);
      if (!match) return null;
      const start = Number(match[1]);
      const end = match[2] ? Number(match[2]) : null;
      if (!Number.isSafeInteger(start) || start < 1 || start > 1000000 ||
          (end !== null && (!Number.isSafeInteger(end) || end < start ||
            end > 1000000))) return null;
      normalized.push(end === null ? String(start) : `${start}-${end}`);
    }
    return normalized.join(",");
  }

  function printPageSettingsFromControls() {
    const pageMode = $('input[name="pdf-print-pages"]:checked')?.value === "custom"
      ? "custom" : "all";
    const rawPageRange = $("#pdf-page-range-input").value;
    return {
      pageMode,
      rawPageRange,
      pageRange: pageMode === "custom" ? normalizePdfPageRange(rawPageRange) : ""
    };
  }

  function printDeviceSettingsFromControls() {
    const copies = Number($("#pdf-print-copies").value);
    return {
      printerName: $("#pdf-printer-select").value || "",
      copies: Number.isInteger(copies) && copies >= 1 && copies <= 999
        ? copies : null
    };
  }

  function storePrintSettings() {
    const pages = printPageSettingsFromControls();
    const device = printDeviceSettingsFromControls();
    localStorage.setItem("mdviewer.printSettings", JSON.stringify({
      pageMode: pages.pageMode,
      pageRange: pages.rawPageRange,
      printerName: device.printerName || preferredPrinterName,
      copies: device.copies || 1
    }));
  }

  function pdfSettingsFromControls() {
    const printPages = printPageSettingsFromControls();
    return {
      paper: ["a4", "letter"].includes($("#pdf-paper-select").value)
        ? $("#pdf-paper-select").value : "a4",
      orientation: $('input[name="pdf-orientation"]:checked')?.value === "landscape"
        ? "landscape" : "portrait",
      marginMm: [0, 10, 20].includes(Number($("#pdf-margin-select").value))
        ? Number($("#pdf-margin-select").value) : 20,
      pageNumbers: $("#pdf-page-numbers").checked,
      printBackground: $("#pdf-print-background").checked,
      pageRanges: pdfDialogMode === "print" && printPages.pageMode === "custom"
        ? printPages.pageRange : ""
    };
  }

  function applyPdfSettingsToControls(settings) {
    $("#pdf-paper-select").value = settings.paper;
    const orientation = $(`input[name="pdf-orientation"][value="${settings.orientation}"]`);
    if (orientation) orientation.checked = true;
    $("#pdf-margin-select").value = String(settings.marginMm);
    $("#pdf-page-numbers").checked = settings.pageNumbers;
    $("#pdf-print-background").checked = settings.printBackground;
    updatePdfPaperSummary();
  }

  function applyPrintSettingsToControls(settings) {
    const mode = settings.pageMode === "custom" ? "custom" : "all";
    const radio = $(`input[name="pdf-print-pages"][value="${mode}"]`);
    if (radio) radio.checked = true;
    $("#pdf-page-range-input").value = settings.pageRange || "";
    $("#pdf-print-copies").value = String(settings.copies || 1);
    preferredPrinterName = settings.printerName || "";
    updatePrintPageRangeControls();
    updatePdfPaperSummary();
  }

  function updatePrintPageRangeControls() {
    const settings = printPageSettingsFromControls();
    const input = $("#pdf-page-range-input");
    input.disabled = settings.pageMode !== "custom";
    const invalid = settings.pageMode === "custom" && settings.pageRange === null;
    input.setAttribute("aria-invalid", invalid ? "true" : "false");
    $("#pdf-page-range-error").hidden = !invalid;
    return !invalid;
  }

  function validPrintDeviceSelection() {
    const device = printDeviceSettingsFromControls();
    $("#pdf-print-copies").setAttribute(
      "aria-invalid", device.copies === null ? "true" : "false");
    return pdfPrintersReady && Boolean(device.printerName) &&
      device.copies !== null;
  }

  function updatePdfActionAvailability() {
    $("#pdf-export-save").disabled = pdfPrintBusy || pdfPrinterPropertiesBusy ||
      pdfPreviewState !== "ready" ||
      (pdfDialogMode === "print" &&
        (!pdfPreviewFrameLoaded || !validPrintDeviceSelection()));
  }

  function setPdfPrintBusy(busy) {
    pdfPrintBusy = busy;
    $$('[data-pdf-export-cancel]').forEach((button) => {
      button.disabled = busy;
    });
    $("#pdf-printer-select").disabled = busy || pdfPrinterPropertiesBusy ||
      !pdfPrintersReady;
    $("#pdf-print-copies").disabled = busy || pdfPrinterPropertiesBusy;
    updatePrinterPropertiesAvailability();
    updatePdfActionAvailability();
  }

  function showPrinterPropertiesStatus(message, state = "") {
    const status = $("#pdf-printer-properties-status");
    status.textContent = message || "";
    status.dataset.state = state;
    status.hidden = !message;
  }

  function updatePrinterPropertiesAvailability() {
    const selected = $("#pdf-printer-select").value || "";
    $("#pdf-printer-properties").disabled = pdfPrintBusy ||
      pdfPrinterPropertiesBusy || !pdfPrintersReady || !selected;
    if (!pdfPrinterPropertiesBusy) {
      showPrinterPropertiesStatus(
        advancedSettingsPrinterName && advancedSettingsPrinterName === selected
          ? i18n.t("Advanced printer settings applied") : "",
        advancedSettingsPrinterName === selected ? "applied" : "");
    }
  }

  function setPrinterPropertiesBusy(busy) {
    pdfPrinterPropertiesBusy = busy;
    $("#pdf-printer-select").disabled = busy || pdfPrintBusy || !pdfPrintersReady;
    $("#pdf-print-copies").disabled = busy || pdfPrintBusy;
    if (busy) {
      showPrinterPropertiesStatus(i18n.t("Opening printer settings…"));
    }
    updatePrinterPropertiesAvailability();
    updatePdfActionAvailability();
  }

  function requestPrinterList() {
    pdfPrintersReady = false;
    advancedSettingsPrinterName = "";
    const select = $("#pdf-printer-select");
    select.disabled = true;
    $("#pdf-printer-properties").disabled = true;
    showPrinterPropertiesStatus("");
    select.replaceChildren(new Option(i18n.t("Loading printers…"), ""));
    $("#pdf-printer-status").hidden = true;
    updatePdfActionAvailability();
    if (!post("printer.list")) {
      select.replaceChildren(new Option(i18n.t("No printers available"), ""));
      $("#pdf-printer-status").textContent = i18n.t("No printers available");
      $("#pdf-printer-status").hidden = false;
    }
  }

  function applyPrinterList(entries) {
    if (pdfDialogMode !== "print" || !$("#pdf-export-dialog").open) return;
    const printers = Array.isArray(entries) ? entries.filter((entry) =>
      entry && typeof entry.name === "string" && entry.name &&
      entry.name.length <= 1024) : [];
    const select = $("#pdf-printer-select");
    select.replaceChildren();
    printers.forEach((entry) => select.add(new Option(entry.name, entry.name)));
    const preferred = printers.find((entry) => entry.name === preferredPrinterName) ||
      printers.find((entry) => entry.isDefault) || printers[0];
    if (preferred) select.value = preferred.name;
    pdfPrintersReady = printers.length > 0;
    select.disabled = !pdfPrintersReady || pdfPrintBusy;
    $("#pdf-printer-status").textContent = pdfPrintersReady
      ? "" : i18n.t("No printers available");
    $("#pdf-printer-status").hidden = pdfPrintersReady;
    if (preferred) preferredPrinterName = preferred.name;
    storePrintSettings();
    updatePrinterPropertiesAvailability();
    updatePdfActionAvailability();
  }

  function updatePdfPaperSummary() {
    const settings = pdfSettingsFromControls();
    const paper = settings.paper === "letter" ? "Letter" : "A4";
    const orientation = i18n.t(settings.orientation === "landscape"
      ? "Landscape" : "Portrait");
    let summary = i18n.t(
      "{paper} · {orientation} · {margin} mm margins",
      { paper, orientation, margin: settings.marginMm });
    if (pdfDialogMode === "print") {
      const pages = printPageSettingsFromControls();
      summary += pages.pageMode === "custom" && pages.pageRange
        ? ` · ${i18n.t("Pages {range}", { range: pages.pageRange })}`
        : ` · ${i18n.t("All pages")}`;
    }
    $("#pdf-paper-summary").textContent = summary;
  }

  function synchronizeDocumentForPdf() {
    if (activeEditorMode() === "source") {
      state.text = sourceEditor.value;
      renderPreview();
    } else if (state.previewChanged) {
      state.text = previewToMarkdown();
      sourceEditor.value = state.text;
      updateSourceHighlight();
    }
  }

  async function waitForPdfResources() {
    const waits = [];
    if (document.fonts?.ready) waits.push(document.fonts.ready.catch(() => {}));
    previewEditor.querySelectorAll("img").forEach((image) => {
      if (image.complete) return;
      waits.push(image.decode?.().catch(() => {}) ||
        new Promise((resolveImage) => {
          image.addEventListener("load", resolveImage, { once: true });
          image.addEventListener("error", resolveImage, { once: true });
        }));
    });
    await Promise.race([
      Promise.all(waits),
      new Promise((resolveTimeout) => setTimeout(resolveTimeout, 3000))
    ]);
    await new Promise((resolveFrame) => requestAnimationFrame(() =>
      requestAnimationFrame(resolveFrame)));
  }

  function showPdfPreviewState(kind) {
    pdfPreviewState = kind;
    $("#pdf-preview-loading").hidden = kind !== "loading";
    $("#pdf-preview-error").hidden = kind !== "error";
    $("#pdf-preview-frame").hidden = kind !== "ready";
    updatePdfActionAvailability();
    $("#pdf-export-status").textContent = i18n.t(
      kind === "ready" ? "Preview ready" :
      kind === "error" ? "Preview unavailable" : "Creating print preview…");
  }

  async function requestPdfPreview() {
    const dialog = $("#pdf-export-dialog");
    if (!dialog.open) return;
    const requestId = ++pdfPreviewSequence;
    activePdfPreviewRequest = requestId;
    if (pdfDialogMode === "print" && !updatePrintPageRangeControls()) {
      pdfPreviewState = "rangeError";
      $("#pdf-preview-loading").hidden = true;
      $("#pdf-preview-error").hidden = true;
      $("#pdf-preview-frame").hidden = true;
      $("#pdf-export-save").disabled = true;
      $("#pdf-export-status").textContent = i18n.t("Enter a valid page range");
      updatePdfPaperSummary();
      return;
    }
    const settings = pdfSettingsFromControls();
    localStorage.setItem("mdviewer.pdfSettings", JSON.stringify(settings));
    if (pdfDialogMode === "print") {
      storePrintSettings();
    }
    document.documentElement.style.setProperty(
      "--pdf-page-margin", `${settings.marginMm}mm`);
    document.documentElement.style.setProperty(
      "--pdf-page-bottom-margin",
      `${settings.pageNumbers ? Math.max(10, settings.marginMm) : settings.marginMm}mm`);
    updatePdfPaperSummary();
    showPdfPreviewState("loading");
    synchronizeDocumentForPdf();
    document.title = state.name || "MdViewer";
    await waitForPdfResources();
    if (!dialog.open || requestId !== activePdfPreviewRequest) return;
    if (!post("pdf.preview", { requestId, ...settings })) {
      showPdfPreviewState("error");
    }
  }

  function schedulePdfPreview(delay = 220) {
    clearTimeout(pdfPreviewTimer);
    pdfPreviewTimer = setTimeout(() => requestPdfPreview(), delay);
  }

  function openPdfDialog(mode) {
    const dialog = $("#pdf-export-dialog");
    if (dialog.open) return;
    pdfDialogMode = mode === "print" ? "print" : "export";
    pdfPreviewFrameLoaded = false;
    pdfPreviewState = "loading";
    pdfPrintersReady = false;
    pdfPrinterPropertiesBusy = false;
    advancedSettingsPrinterName = "";
    setPdfPrintBusy(false);
    const printing = pdfDialogMode === "print";
    $("[data-export-format-field]").hidden = printing;
    if (!printing) setExportFormatSelection("pdf");
    $("#pdf-export-dialog-title").textContent = i18n.t(printing ? "Print" : "Export PDF");
    $("#pdf-export-dialog-title").nextElementSibling.textContent = i18n.t(printing
      ? "Choose a printer, copies, and pages, then review before printing."
      : "Review the final pages before saving.");
    $("#pdf-page-range-settings").hidden = !printing;
    $("#pdf-printer-settings").hidden = !printing;
    $("#pdf-export-save").textContent = i18n.t(printing ? "Print…" : "Save PDF");
    applyPdfSettingsToControls(readStoredPdfSettings());
    applyPrintSettingsToControls(readStoredPrintSettings());
    $("#pdf-preview-frame").removeAttribute("src");
    dialog.showModal();
    if (printing) requestPrinterList();
    schedulePdfPreview(0);
  }

  function openPdfExportDialog() { openPdfDialog("export"); }
  function openPrintDialog() { openPdfDialog("print"); }

  function normalizedExportFormat(value) {
    return ["pdf", "docx", "hwpx"].includes(value) ? value : "pdf";
  }

  function storedExportFormat() {
    return normalizedExportFormat(localStorage.getItem("mdviewer.exportFormat"));
  }

  function setExportFormatSelection(format) {
    const normalized = normalizedExportFormat(format);
    localStorage.setItem("mdviewer.exportFormat", normalized);
    $$('[data-export-format-select]').forEach((select) => {
      select.value = normalized;
    });
    return normalized;
  }

  function openExportDialog(format = storedExportFormat()) {
    const normalized = setExportFormatSelection(format);
    if (normalized === "docx") openDocxExportDialog();
    else if (normalized === "hwpx") openHwpxExportDialog();
    else openPdfExportDialog();
  }

  function switchExportFormat(format) {
    const normalized = normalizedExportFormat(format);
    const current = $("#pdf-export-dialog").open && pdfDialogMode === "export"
      ? "pdf" : $("#docx-export-dialog").open
        ? "docx" : $("#hwpx-export-dialog").open ? "hwpx" : null;
    if (current === normalized) {
      setExportFormatSelection(normalized);
      return;
    }
    if (docxExportBusy || hwpxExportBusy) {
      if (current) setExportFormatSelection(current);
      return;
    }
    if (current === "pdf") closePdfExportDialog();
    else if (current === "docx") closeDocxExportDialog();
    else if (current === "hwpx") closeHwpxExportDialog();
    openExportDialog(normalized);
  }

  function closePdfExportDialog() {
    const dialog = $("#pdf-export-dialog");
    if (!dialog.open || pdfPrintBusy) return;
    clearTimeout(pdfPreviewTimer);
    activePdfPreviewRequest = ++pdfPreviewSequence;
    pdfPreviewFrameLoaded = false;
    $("#pdf-preview-frame").removeAttribute("src");
    dialog.close("cancel");
    post("pdf.previewClose");
  }

  function readStoredDocxSettings() {
    const defaults = {
      paper: "a4", orientation: "portrait", marginMm: 20,
      font: "sans", includeImages: true, author: ""
    };
    try {
      const stored = JSON.parse(localStorage.getItem("mdviewer.docxSettings") || "null");
      if (!stored || typeof stored !== "object") return defaults;
      return {
        paper: ["a4", "letter"].includes(stored.paper) ? stored.paper : defaults.paper,
        orientation: ["portrait", "landscape"].includes(stored.orientation)
          ? stored.orientation : defaults.orientation,
        marginMm: [0, 10, 20].includes(Number(stored.marginMm))
          ? Number(stored.marginMm) : defaults.marginMm,
        font: ["serif", "sans"].includes(stored.font) ? stored.font : defaults.font,
        includeImages: stored.includeImages !== false,
        author: typeof stored.author === "string" ? stored.author.slice(0, 120) : ""
      };
    } catch {
      return defaults;
    }
  }

  function docxSettingsFromControls() {
    return {
      paper: ["a4", "letter"].includes($("#docx-paper-select").value)
        ? $("#docx-paper-select").value : "a4",
      orientation: $('input[name="docx-orientation"]:checked')?.value === "landscape"
        ? "landscape" : "portrait",
      marginMm: [0, 10, 20].includes(Number($("#docx-margin-select").value))
        ? Number($("#docx-margin-select").value) : 20,
      font: $("#docx-font-select").value === "serif" ? "serif" : "sans",
      includeImages: $("#docx-include-images").checked,
      title: $("#docx-title-input").value.trim().slice(0, 240),
      author: $("#docx-author-input").value.trim().slice(0, 120)
    };
  }

  function updateDocxSummary() {
    const settings = docxSettingsFromControls();
    const paper = settings.paper === "letter" ? "Letter" : "A4";
    const orientation = i18n.t(settings.orientation === "landscape"
      ? "Landscape" : "Portrait");
    $("#docx-layout-summary").textContent = i18n.t(
      "{paper} · {orientation} · {margin} mm margins",
      { paper, orientation, margin: settings.marginMm });
  }

  function applyDocxSettingsToControls(settings) {
    $("#docx-paper-select").value = settings.paper;
    const orientation = $(`input[name="docx-orientation"][value="${settings.orientation}"]`);
    if (orientation) orientation.checked = true;
    $("#docx-margin-select").value = String(settings.marginMm);
    $("#docx-font-select").value = settings.font;
    $("#docx-include-images").checked = settings.includeImages;
    $("#docx-author-input").value = settings.author;
    const stem = (state.name || i18n.t("Untitled")).replace(/\.[^.]+$/, "");
    $("#docx-title-input").value = stem;
    updateDocxSummary();
  }

  function updateDocxContentPreview() {
    const preview = $("#docx-document-preview");
    const clone = previewEditor.cloneNode(true);
    clone.removeAttribute("id");
    clone.removeAttribute("contenteditable");
    clone.querySelectorAll("[contenteditable]").forEach((element) =>
      element.removeAttribute("contenteditable"));
    clone.querySelectorAll("input").forEach((input) => { input.disabled = true; });
    preview.replaceChildren(...clone.childNodes);
    preview.style.fontFamily = $("#docx-font-select").value === "serif"
      ? 'Batang, "Times New Roman", serif'
      : '"Malgun Gothic", "Segoe UI", sans-serif';
  }

  function openDocxExportDialog() {
    const dialog = $("#docx-export-dialog");
    if (dialog.open) return;
    setExportFormatSelection("docx");
    synchronizeDocumentForPdf();
    applyDocxSettingsToControls(readStoredDocxSettings());
    updateDocxContentPreview();
    docxExportBusy = false;
    $("#docx-export-save").disabled = false;
    $("#docx-export-status").textContent = i18n.t("Ready to export");
    dialog.showModal();
  }

  function closeDocxExportDialog() {
    const dialog = $("#docx-export-dialog");
    if (!dialog.open || docxExportBusy) return;
    dialog.close("cancel");
  }

  function readStoredHwpxSettings() {
    const defaults = {
      paper: "a4", orientation: "portrait", marginMm: 20,
      font: "serif", includeImages: true, author: ""
    };
    try {
      const stored = JSON.parse(localStorage.getItem("mdviewer.hwpxSettings") || "null");
      if (!stored || typeof stored !== "object") return defaults;
      return {
        paper: ["a4", "letter"].includes(stored.paper) ? stored.paper : defaults.paper,
        orientation: ["portrait", "landscape"].includes(stored.orientation)
          ? stored.orientation : defaults.orientation,
        marginMm: [0, 10, 20].includes(Number(stored.marginMm))
          ? Number(stored.marginMm) : defaults.marginMm,
        font: ["serif", "sans"].includes(stored.font) ? stored.font : defaults.font,
        includeImages: stored.includeImages !== false,
        author: typeof stored.author === "string" ? stored.author.slice(0, 120) : ""
      };
    } catch {
      return defaults;
    }
  }

  function hwpxSettingsFromControls() {
    return {
      paper: ["a4", "letter"].includes($("#hwpx-paper-select").value)
        ? $("#hwpx-paper-select").value : "a4",
      orientation: $('input[name="hwpx-orientation"]:checked')?.value === "landscape"
        ? "landscape" : "portrait",
      marginMm: [0, 10, 20].includes(Number($("#hwpx-margin-select").value))
        ? Number($("#hwpx-margin-select").value) : 20,
      font: $("#hwpx-font-select").value === "sans" ? "sans" : "serif",
      includeImages: $("#hwpx-include-images").checked,
      title: $("#hwpx-title-input").value.trim().slice(0, 240),
      author: $("#hwpx-author-input").value.trim().slice(0, 120)
    };
  }

  function updateHwpxSummary() {
    const settings = hwpxSettingsFromControls();
    const paper = settings.paper === "letter" ? "Letter" : "A4";
    const orientation = i18n.t(settings.orientation === "landscape"
      ? "Landscape" : "Portrait");
    $("#hwpx-layout-summary").textContent = i18n.t(
      "{paper} · {orientation} · {margin} mm margins",
      { paper, orientation, margin: settings.marginMm });
  }

  function applyHwpxSettingsToControls(settings) {
    $("#hwpx-paper-select").value = settings.paper;
    const orientation = $(`input[name="hwpx-orientation"][value="${settings.orientation}"]`);
    if (orientation) orientation.checked = true;
    $("#hwpx-margin-select").value = String(settings.marginMm);
    $("#hwpx-font-select").value = settings.font;
    $("#hwpx-include-images").checked = settings.includeImages;
    $("#hwpx-author-input").value = settings.author;
    const stem = (state.name || i18n.t("Untitled")).replace(/\.[^.]+$/, "");
    $("#hwpx-title-input").value = stem;
    updateHwpxSummary();
  }

  function updateHwpxContentPreview() {
    const preview = $("#hwpx-document-preview");
    const clone = previewEditor.cloneNode(true);
    clone.removeAttribute("id");
    clone.removeAttribute("contenteditable");
    clone.querySelectorAll("[contenteditable]").forEach((element) =>
      element.removeAttribute("contenteditable"));
    clone.querySelectorAll("input").forEach((input) => { input.disabled = true; });
    preview.replaceChildren(...clone.childNodes);
    preview.style.fontFamily = $("#hwpx-font-select").value === "sans"
      ? '"함초롬돋움", "Malgun Gothic", sans-serif'
      : '"함초롬바탕", Batang, serif';
  }

  function openHwpxExportDialog() {
    const dialog = $("#hwpx-export-dialog");
    if (dialog.open) return;
    setExportFormatSelection("hwpx");
    synchronizeDocumentForPdf();
    applyHwpxSettingsToControls(readStoredHwpxSettings());
    updateHwpxContentPreview();
    hwpxExportBusy = false;
    $("#hwpx-export-save").disabled = false;
    $("#hwpx-export-status").textContent = i18n.t("Ready to export");
    dialog.showModal();
  }

  function closeHwpxExportDialog() {
    const dialog = $("#hwpx-export-dialog");
    if (!dialog.open || hwpxExportBusy) return;
    dialog.close("cancel");
  }

  function xmlEscape(value) {
    return String(value ?? "")
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&apos;");
  }

  function hwpxUnitFromMillimeters(value) {
    return Math.max(0, Math.round(Number(value) * 7200 / 25.4));
  }

  function hwpxPageMetrics(settings) {
    let width = settings.paper === "letter" ? 61200 : 59528;
    let height = settings.paper === "letter" ? 79200 : 84186;
    if (settings.orientation === "landscape") [width, height] = [height, width];
    const margin = hwpxUnitFromMillimeters(settings.marginMm);
    return {
      width, height, margin,
      contentWidth: Math.max(7200, width - margin * 2),
      contentHeight: Math.max(7200, height - margin * 2)
    };
  }

  function hwpxSectionProperties(settings) {
    const page = hwpxPageMetrics(settings);
    const headerFooter = Math.min(page.margin, hwpxUnitFromMillimeters(10));
    return `<hp:secPr id="" textDirection="HORIZONTAL" spaceColumns="1134" tabStop="8000" outlineShapeIDRef="1" memoShapeIDRef="0" textVerticalWidthHead="0" masterPageCnt="0">` +
      `<hp:grid lineGrid="0" charGrid="0" wonggojiFormat="0"/>` +
      `<hp:startNum pageStartsOn="BOTH" page="0" pic="0" tbl="0" equation="0"/>` +
      `<hp:visibility hideFirstHeader="0" hideFirstFooter="0" hideFirstMasterPage="0" border="SHOW_ALL" fill="SHOW_ALL" hideFirstPageNum="0" hideFirstEmptyLine="0" showLineNumber="0"/>` +
      `<hp:lineNumberShape restartType="0" countBy="0" distance="0" startNumber="0"/>` +
      `<hp:pagePr landscape="WIDELY" width="${page.width}" height="${page.height}" gutterType="LEFT_ONLY">` +
      `<hp:margin header="${headerFooter}" footer="${headerFooter}" gutter="0" left="${page.margin}" right="${page.margin}" top="${page.margin}" bottom="${page.margin}"/>` +
      `</hp:pagePr><hp:footNotePr><hp:autoNumFormat type="DIGIT" userChar="" prefixChar="" suffixChar=")" supscript="0"/>` +
      `<hp:noteLine length="-1" type="SOLID" width="0.12 mm" color="#000000"/><hp:noteSpacing betweenNotes="283" belowLine="567" aboveLine="850"/>` +
      `<hp:numbering type="CONTINUOUS" newNum="1"/><hp:placement place="EACH_COLUMN" beneathText="0"/></hp:footNotePr>` +
      `<hp:endNotePr><hp:autoNumFormat type="DIGIT" userChar="" prefixChar="" suffixChar=")" supscript="0"/>` +
      `<hp:noteLine length="14692344" type="SOLID" width="0.12 mm" color="#000000"/><hp:noteSpacing betweenNotes="0" belowLine="567" aboveLine="850"/>` +
      `<hp:numbering type="CONTINUOUS" newNum="1"/><hp:placement place="END_OF_DOCUMENT" beneathText="0"/></hp:endNotePr>` +
      `<hp:pageBorderFill type="BOTH" borderFillIDRef="1" textBorder="PAPER" headerInside="0" footerInside="0" fillArea="PAPER"><hp:offset left="1417" right="1417" top="1417" bottom="1417"/></hp:pageBorderFill>` +
      `<hp:pageBorderFill type="EVEN" borderFillIDRef="1" textBorder="PAPER" headerInside="0" footerInside="0" fillArea="PAPER"><hp:offset left="1417" right="1417" top="1417" bottom="1417"/></hp:pageBorderFill>` +
      `<hp:pageBorderFill type="ODD" borderFillIDRef="1" textBorder="PAPER" headerInside="0" footerInside="0" fillArea="PAPER"><hp:offset left="1417" right="1417" top="1417" bottom="1417"/></hp:pageBorderFill>` +
      `</hp:secPr><hp:ctrl><hp:colPr id="" type="NEWSPAPER" layout="LEFT" colCount="1" sameSz="1" sameGap="0"/></hp:ctrl>`;
  }

  function hwpxCharId(style, headingLevel = 0, tableHeader = false) {
    if (tableHeader) return 20;
    if (headingLevel) return 13 + Math.min(6, Math.max(1, headingLevel));
    if (style.code) return 12;
    if (style.link) return 13;
    if (style.strike) return 11;
    if (style.bold && style.italic) return 10;
    if (style.bold) return 8;
    if (style.italic) return 9;
    return 7;
  }

  function collectHwpxRuns(node, runs, style = {}, headingLevel = 0,
    imageMap = new Map(), tableHeader = false) {
    if (node.nodeType === Node.TEXT_NODE) {
      const text = (node.nodeValue || "").replace(/\s*\n\s*/g, " ");
      if (text) runs.push({ text, charId: hwpxCharId(style, headingLevel, tableHeader) });
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const tag = node.tagName.toLowerCase();
    if (tag === "br") {
      runs.push({ text: " ", charId: hwpxCharId(style, headingLevel, tableHeader) });
      return;
    }
    if (tag === "input") return;
    if (tag === "img") {
      const prepared = imageMap.get(node);
      if (prepared) runs.push({ image: prepared });
      else runs.push({
        text: `[${i18n.t("Image")}: ${node.getAttribute("alt") || i18n.t("Unavailable")}]`,
        charId: 9
      });
      return;
    }
    const next = { ...style };
    if (["strong", "b"].includes(tag)) next.bold = true;
    if (["em", "i"].includes(tag)) next.italic = true;
    if (["del", "s", "strike"].includes(tag)) next.strike = true;
    if (tag === "code") next.code = true;
    if (tag === "a") next.link = true;
    [...node.childNodes].forEach((child) =>
      collectHwpxRuns(child, runs, next, headingLevel, imageMap, tableHeader));
    if (tag === "a") {
      const href = node.dataset.mdHref || node.getAttribute("href") || "";
      if (href && href !== node.textContent) {
        runs.push({ text: ` (${href})`, charId: 13 });
      }
    }
  }

  function hwpxPictureXml(image) {
    const width = image.width;
    const height = image.height;
    const comment = image.alt ? `<hp:shapeComment>${xmlEscape(image.alt)}</hp:shapeComment>` : "";
    return `<hp:pic id="${image.shapeId}" zOrder="0" numberingType="PICTURE" textWrap="TOP_AND_BOTTOM" textFlow="BOTH_SIDES" lock="0" dropcapstyle="None" href="" groupLevel="0" instid="${image.shapeId}" reverse="0">` +
      `<hp:offset x="0" y="0"/><hp:orgSz width="${width}" height="${height}"/><hp:curSz width="${width}" height="${height}"/>` +
      `<hp:flip horizontal="0" vertical="0"/><hp:rotationInfo angle="0" centerX="${Math.round(width / 2)}" centerY="${Math.round(height / 2)}" rotateimage="1"/>` +
      `<hp:renderingInfo><hc:transMatrix e1="1" e2="0" e3="0" e4="0" e5="1" e6="0"/><hc:scaMatrix e1="1" e2="0" e3="0" e4="0" e5="1" e6="0"/><hc:rotMatrix e1="1" e2="0" e3="0" e4="0" e5="1" e6="0"/></hp:renderingInfo>` +
      `<hp:imgRect><hc:pt0 x="0" y="0"/><hc:pt1 x="${width}" y="0"/><hc:pt2 x="${width}" y="${height}"/><hc:pt3 x="0" y="${height}"/></hp:imgRect>` +
      `<hp:imgClip left="0" right="${width}" top="0" bottom="${height}"/><hp:inMargin left="0" right="0" top="0" bottom="0"/>` +
      `<hc:img binaryItemIDRef="${image.id}" bright="0" contrast="0" effect="REAL_PIC" alpha="0"/><hp:effects/>` +
      `<hp:sz width="${width}" widthRelTo="ABSOLUTE" height="${height}" heightRelTo="ABSOLUTE" protect="0"/>` +
      `<hp:pos treatAsChar="1" affectLSpacing="0" flowWithText="1" allowOverlap="0" holdAnchorAndSO="0" vertRelTo="PARA" horzRelTo="COLUMN" vertAlign="TOP" horzAlign="LEFT" vertOffset="0" horzOffset="0"/>` +
      `<hp:outMargin left="0" right="0" top="0" bottom="0"/>${comment}</hp:pic><hp:t/>`;
  }

  function hwpxRunsXml(runs) {
    if (!runs.length) return `<hp:run charPrIDRef="7"><hp:t/></hp:run>`;
    return runs.map((run) => run.image
      ? `<hp:run charPrIDRef="7">${hwpxPictureXml(run.image)}</hp:run>`
      : `<hp:run charPrIDRef="${run.charId}"><hp:t>${xmlEscape(run.text)}</hp:t></hp:run>`).join("");
  }

  function prepareDocumentImages(settings) {
    const imageMap = new Map();
    const records = [];
    const allImages = [...previewEditor.querySelectorAll("img")];
    let skipped = 0;
    let totalCharacters = 0;
    if (!settings.includeImages) return Promise.resolve({ imageMap, records, skipped });
    skipped = Math.max(0, allImages.length - 128);
    const page = hwpxPageMetrics(settings);
    const images = allImages.slice(0, 128);
    return images.reduce((promise, image, index) => promise.then(async () => {
      try {
        if (!image.complete || !image.naturalWidth || !image.naturalHeight) {
          throw new Error("unavailable");
        }
        if (image.naturalWidth > 16384 || image.naturalHeight > 16384 ||
            image.naturalWidth * image.naturalHeight > 64 * 1024 * 1024) {
          throw new Error("dimensions");
        }
        const canvas = document.createElement("canvas");
        canvas.width = image.naturalWidth;
        canvas.height = image.naturalHeight;
        const context = canvas.getContext("2d", { alpha: true });
        if (!context) throw new Error("canvas");
        context.drawImage(image, 0, 0);
        const dataUrl = canvas.toDataURL("image/png");
        totalCharacters += dataUrl.length;
        if (dataUrl.length > 24 * 1024 * 1024 || totalCharacters > 80 * 1024 * 1024) {
          throw new Error("large");
        }
        const naturalWidth = Math.max(1, image.naturalWidth * 75);
        const naturalHeight = Math.max(1, image.naturalHeight * 75);
        const scale = Math.min(1, page.contentWidth / naturalWidth,
          page.contentHeight * 0.8 / naturalHeight);
        const prepared = {
          id: `image${index + 1}`,
          shapeId: 2000000000 + index + 1,
          width: Math.max(720, Math.round(naturalWidth * scale)),
          height: Math.max(720, Math.round(naturalHeight * scale)),
          alt: image.getAttribute("alt") || ""
        };
        imageMap.set(image, prepared);
        records.push(`${prepared.id}\t${dataUrl}`);
      } catch {
        skipped += 1;
      }
    }), Promise.resolve()).then(() => ({ imageMap, records, skipped }));
  }

  function buildHwpxSection(settings, imageMap) {
    const namespace = `xmlns:ha="http://www.hancom.co.kr/hwpml/2011/app" xmlns:hp="http://www.hancom.co.kr/hwpml/2011/paragraph" xmlns:hp10="http://www.hancom.co.kr/hwpml/2016/paragraph" xmlns:hs="http://www.hancom.co.kr/hwpml/2011/section" xmlns:hc="http://www.hancom.co.kr/hwpml/2011/core" xmlns:hh="http://www.hancom.co.kr/hwpml/2011/head" xmlns:hpf="http://www.hancom.co.kr/schema/2011/hpf" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf/" xmlns:config="urn:oasis:names:tc:opendocument:xmlns:config:1.0"`;
    const page = hwpxPageMetrics(settings);
    let nextId = 1000000001;
    let firstParagraph = true;
    const paragraph = (runs, paraPr = 0, headingLevel = 0) => {
      const section = firstParagraph
        ? `<hp:run charPrIDRef="7">${hwpxSectionProperties(settings)}</hp:run>` : "";
      firstParagraph = false;
      return `<hp:p id="${nextId++}" paraPrIDRef="${paraPr}" styleIDRef="0" pageBreak="0" columnBreak="0" merged="0">${section}${hwpxRunsXml(runs, headingLevel)}</hp:p>`;
    };
    const runsFor = (element, headingLevel = 0, tableHeader = false) => {
      const runs = [];
      [...element.childNodes].forEach((child) =>
        collectHwpxRuns(child, runs, {}, headingLevel, imageMap, tableHeader));
      return runs;
    };
    const blocks = [];

    const renderList = (list, depth = 0) => {
      const ordered = list.tagName.toLowerCase() === "ol";
      [...list.children].filter((child) => child.tagName?.toLowerCase() === "li")
        .forEach((item, index) => {
          const checkbox = item.querySelector(":scope > input[type='checkbox']");
          const prefix = checkbox ? (checkbox.checked ? "☑ " : "☐ ")
            : ordered ? `${index + 1}. ` : "• ";
          const runs = [{ text: `${"  ".repeat(depth)}${prefix}`, charId: 7 }];
          [...item.childNodes]
            .filter((child) => !(child.nodeType === Node.ELEMENT_NODE &&
              ["ul", "ol", "input"].includes(child.tagName.toLowerCase())))
            .forEach((child) => collectHwpxRuns(child, runs, {}, 0, imageMap));
          blocks.push(paragraph(runs, 22));
          [...item.children].filter((child) => ["ul", "ol"].includes(child.tagName.toLowerCase()))
            .forEach((nested) => renderList(nested, depth + 1));
        });
    };

    const renderTable = (table) => {
      const rows = [...table.querySelectorAll(":scope > thead > tr, :scope > tbody > tr, :scope > tr")];
      if (!rows.length) return;
      const colCount = Math.max(...rows.map((row) => row.children.length), 1);
      const colWidth = Math.floor(page.contentWidth / colCount);
      const rowHeight = 2400;
      const section = firstParagraph
        ? `<hp:run charPrIDRef="7">${hwpxSectionProperties(settings)}</hp:run>` : "";
      firstParagraph = false;
      let xml = `<hp:p id="${nextId++}" paraPrIDRef="0" styleIDRef="0" pageBreak="0" columnBreak="0" merged="0">${section}<hp:run charPrIDRef="7">` +
        `<hp:tbl id="${nextId++}" zOrder="0" numberingType="TABLE" textWrap="TOP_AND_BOTTOM" textFlow="BOTH_SIDES" lock="0" dropcapstyle="None" pageBreak="CELL" repeatHeader="1" rowCnt="${rows.length}" colCnt="${colCount}" cellSpacing="0" borderFillIDRef="3" noAdjust="0">` +
        `<hp:sz width="${page.contentWidth}" widthRelTo="ABSOLUTE" height="${rowHeight * rows.length}" heightRelTo="ABSOLUTE" protect="0"/>` +
        `<hp:pos treatAsChar="1" affectLSpacing="0" flowWithText="1" allowOverlap="0" holdAnchorAndSO="0" vertRelTo="PARA" horzRelTo="COLUMN" vertAlign="TOP" horzAlign="LEFT" vertOffset="0" horzOffset="0"/>` +
        `<hp:outMargin left="0" right="0" top="283" bottom="283"/><hp:inMargin left="420" right="420" top="180" bottom="180"/>`;
      rows.forEach((row, rowIndex) => {
        xml += "<hp:tr>";
        for (let column = 0; column < colCount; column += 1) {
          const cell = row.children[column];
          const header = rowIndex === 0 || cell?.tagName.toLowerCase() === "th";
          const runs = cell ? runsFor(cell, 0, header) : [];
          xml += `<hp:tc name="" header="${header ? 1 : 0}" hasMargin="0" protect="0" editable="0" dirty="0" borderFillIDRef="3">` +
            `<hp:subList id="" textDirection="HORIZONTAL" lineWrap="BREAK" vertAlign="CENTER" linkListIDRef="0" linkListNextIDRef="0" textWidth="0" textHeight="0" hasTextRef="0" hasNumRef="0">` +
            `<hp:p id="${nextId++}" paraPrIDRef="23" styleIDRef="0" pageBreak="0" columnBreak="0" merged="0">${hwpxRunsXml(runs)}</hp:p></hp:subList>` +
            `<hp:cellAddr colAddr="${column}" rowAddr="${rowIndex}"/><hp:cellSpan colSpan="1" rowSpan="1"/>` +
            `<hp:cellSz width="${column === colCount - 1 ? page.contentWidth - colWidth * (colCount - 1) : colWidth}" height="${rowHeight}"/>` +
            `<hp:cellMargin left="420" right="420" top="180" bottom="180"/></hp:tc>`;
        }
        xml += "</hp:tr>";
      });
      xml += "</hp:tbl><hp:t/></hp:run></hp:p>";
      blocks.push(xml);
    };

    const renderBlock = (element, forcedParaPr = null) => {
      const tag = element.tagName.toLowerCase();
      if (/^h[1-6]$/.test(tag)) {
        const level = Number(tag[1]);
        blocks.push(paragraph(runsFor(element, level), level + 1, level));
      } else if (tag === "p" || tag === "div") {
        blocks.push(paragraph(runsFor(element), forcedParaPr ??
          (element.classList.contains("protected-block") ? 21 : 0)));
      } else if (tag === "pre") {
        const lines = String(element.textContent || "").replaceAll("\r\n", "\n").split("\n");
        lines.forEach((line) => blocks.push(paragraph([
          { text: line || " ", charId: 12 }
        ], 21)));
      } else if (tag === "blockquote") {
        if (element.children.length) [...element.children].forEach((child) => renderBlock(child, 20));
        else blocks.push(paragraph(runsFor(element), 20));
      } else if (tag === "ul" || tag === "ol") {
        renderList(element);
      } else if (tag === "table") {
        renderTable(element);
      } else if (tag === "hr") {
        blocks.push(paragraph([], 24));
      } else {
        blocks.push(paragraph(runsFor(element), forcedParaPr ?? 0));
      }
    };

    [...previewEditor.children].forEach((element) => renderBlock(element));
    if (!blocks.length) blocks.push(paragraph([], 0));
    return `<?xml version="1.0" encoding="UTF-8" standalone="yes" ?><hs:sec ${namespace}>${blocks.join("")}</hs:sec>`;
  }

  async function buildHwpxExportPayload() {
    synchronizeDocumentForPdf();
    await waitForPdfResources();
    const settings = hwpxSettingsFromControls();
    localStorage.setItem("mdviewer.hwpxSettings", JSON.stringify({
      paper: settings.paper,
      orientation: settings.orientation,
      marginMm: settings.marginMm,
      font: settings.font,
      includeImages: settings.includeImages,
      author: settings.author
    }));
    const prepared = await prepareDocumentImages(settings);
    return {
      ...settings,
      sectionXml: buildHwpxSection(settings, prepared.imageMap),
      previewText: previewEditor.innerText || previewEditor.textContent || "",
      images: prepared.records.join("\n"),
      skippedImages: prepared.skipped
    };
  }

  function docxPageMetrics(settings) {
    let width = settings.paper === "letter" ? 12240 : 11906;
    let height = settings.paper === "letter" ? 15840 : 16838;
    if (settings.orientation === "landscape") [width, height] = [height, width];
    const margin = Math.max(0, Math.round(settings.marginMm * 1440 / 25.4));
    return {
      width, height, margin,
      contentWidth: Math.max(1440, width - margin * 2),
      contentHeight: Math.max(1440, height - margin * 2)
    };
  }

  function docxSafeText(value) {
    return String(value ?? "").replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F]/g, "");
  }

  function docxSafeHyperlink(value) {
    const target = docxSafeText(value).trim();
    return /^(?:https?:\/\/|mailto:)/i.test(target) &&
      !/[\u0000-\u001F\u007F]/.test(target) && target.length <= 8192
      ? target : "";
  }

  function collectDocxRuns(node, runs, style, imageMap) {
    if (node.nodeType === Node.TEXT_NODE) {
      const text = docxSafeText(node.nodeValue).replace(/\s*\n\s*/g, " ");
      if (text) runs.push({ text, ...style });
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const tag = node.tagName.toLowerCase();
    if (tag === "br") {
      runs.push({ break: true });
      return;
    }
    if (tag === "input") return;
    if (tag === "img") {
      const image = imageMap.get(node);
      if (image) runs.push({ image });
      else runs.push({
        text: `[${i18n.t("Image")}: ${node.getAttribute("alt") || i18n.t("Unavailable")}]`,
        italic: true
      });
      return;
    }
    const next = { ...style };
    if (["strong", "b"].includes(tag)) next.bold = true;
    if (["em", "i"].includes(tag)) next.italic = true;
    if (["del", "s", "strike"].includes(tag)) next.strike = true;
    if (tag === "code") next.code = true;
    if (tag === "a") {
      const href = node.dataset.mdHref || node.getAttribute("href") || "";
      next.link = docxSafeHyperlink(href);
    }
    [...node.childNodes].forEach((child) =>
      collectDocxRuns(child, runs, next, imageMap));
  }

  function docxTextRunXml(run) {
    if (run.break) return "<w:r><w:br/></w:r>";
    const properties = [];
    if (run.bold) properties.push("<w:b/><w:bCs/>");
    if (run.italic) properties.push("<w:i/><w:iCs/>");
    if (run.strike) properties.push("<w:strike/>");
    if (run.code) properties.push(
      '<w:rFonts w:ascii="Consolas" w:hAnsi="Consolas" w:eastAsia="Malgun Gothic"/>',
      '<w:shd w:val="clear" w:fill="F1F3F5"/>',
      '<w:sz w:val="19"/><w:szCs w:val="19"/>');
    if (run.link) properties.push('<w:rStyle w:val="Hyperlink"/>');
    const text = docxSafeText(run.text);
    return `<w:r>${properties.length ? `<w:rPr>${properties.join("")}</w:rPr>` : ""}` +
      `<w:t xml:space="preserve">${xmlEscape(text)}</w:t></w:r>`;
  }

  function docxPictureXml(image, context) {
    const width = Math.max(91440, Math.round(image.width * 127));
    const height = Math.max(91440, Math.round(image.height * 127));
    const relationId = `rIdImage${image.id.slice(5)}`;
    const drawingId = context.nextDrawingId++;
    const alt = docxSafeText(image.alt).slice(0, 1024);
    return `<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0" ` +
      `xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing">` +
      `<wp:extent cx="${width}" cy="${height}"/><wp:effectExtent l="0" t="0" r="0" b="0"/>` +
      `<wp:docPr id="${drawingId}" name="${xmlEscape(image.id)}" descr="${xmlEscape(alt)}"/>` +
      `<wp:cNvGraphicFramePr><a:graphicFrameLocks xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" noChangeAspect="1"/></wp:cNvGraphicFramePr>` +
      `<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">` +
      `<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">` +
      `<pic:nvPicPr><pic:cNvPr id="0" name="${xmlEscape(image.id)}" descr="${xmlEscape(alt)}"/><pic:cNvPicPr><a:picLocks noChangeAspect="1"/></pic:cNvPicPr></pic:nvPicPr>` +
      `<pic:blipFill><a:blip r:embed="${relationId}"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>` +
      `<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="${width}" cy="${height}"/></a:xfrm>` +
      `<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>` +
      `</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>`;
  }

  function docxRunsXml(runs, context) {
    if (!runs.length) return "<w:r><w:t/></w:r>";
    return runs.map((run) => {
      if (run.image) return docxPictureXml(run.image, context);
      const runXml = docxTextRunXml(run);
      if (!run.link) return runXml;
      let id = context.linkByTarget.get(run.link);
      if (!id) {
        id = `link${context.hyperlinks.length + 1}`;
        context.linkByTarget.set(run.link, id);
        context.hyperlinks.push({ id, target: run.link });
      }
      return `<w:hyperlink r:id="rIdLink${id.slice(4)}" w:history="1">${runXml}</w:hyperlink>`;
    }).join("");
  }

  function buildDocxDocument(settings, imageMap) {
    const page = docxPageMetrics(settings);
    const context = {
      hyperlinks: [],
      linkByTarget: new Map(),
      lists: [],
      nextDrawingId: 1
    };
    const blocks = [];
    const runsFor = (element, initialStyle = {}) => {
      const runs = [];
      [...element.childNodes].forEach((child) =>
        collectDocxRuns(child, runs, initialStyle, imageMap));
      return runs;
    };
    const paragraph = (runs, options = {}) => {
      const properties = [];
      if (options.style) properties.push(`<w:pStyle w:val="${options.style}"/>`);
      if (options.numId) {
        properties.push(`<w:numPr><w:ilvl w:val="${Math.min(8, options.level || 0)}"/>` +
          `<w:numId w:val="${options.numId}"/></w:numPr>`);
      }
      if (options.keepNext) properties.push("<w:keepNext/>");
      if (options.horizontalRule) {
        properties.push('<w:pBdr><w:bottom w:val="single" w:sz="12" w:space="8" w:color="8A929E"/></w:pBdr>');
      }
      return `<w:p>${properties.length ? `<w:pPr>${properties.join("")}</w:pPr>` : ""}` +
        `${docxRunsXml(runs, context)}</w:p>`;
    };

    const renderList = (list, depth = 0) => {
      const ordered = list.tagName.toLowerCase() === "ol";
      const start = ordered
        ? Math.max(1, Math.min(1000000, Number(list.getAttribute("start")) || 1)) : 1;
      context.lists.push({ ordered, start });
      const numId = context.lists.length;
      [...list.children].filter((child) => child.tagName?.toLowerCase() === "li")
        .forEach((item) => {
          const runs = [];
          const checkbox = item.querySelector(":scope > input[type='checkbox']");
          if (checkbox) runs.push({ text: checkbox.checked ? "☑ " : "☐ " });
          [...item.childNodes]
            .filter((child) => !(child.nodeType === Node.ELEMENT_NODE &&
              ["ul", "ol", "input"].includes(child.tagName.toLowerCase())))
            .forEach((child) => collectDocxRuns(child, runs, {}, imageMap));
          blocks.push(paragraph(runs, { numId, level: depth }));
          [...item.children]
            .filter((child) => ["ul", "ol"].includes(child.tagName.toLowerCase()))
            .forEach((nested) => renderList(nested, depth + 1));
        });
    };

    const renderTable = (table) => {
      const rows = [...table.querySelectorAll(
        ":scope > thead > tr, :scope > tbody > tr, :scope > tfoot > tr, :scope > tr")];
      if (!rows.length) return;
      const colCount = Math.max(1, ...rows.map((row) => row.children.length));
      const tableWidth = Math.max(720, page.contentWidth - 120);
      const baseWidth = Math.floor(tableWidth / colCount);
      const widths = Array.from({ length: colCount }, (_, index) =>
        index === colCount - 1 ? tableWidth - baseWidth * (colCount - 1) : baseWidth);
      let xml = `<w:tbl><w:tblPr><w:tblW w:w="${tableWidth}" w:type="dxa"/>` +
        `<w:tblInd w:w="120" w:type="dxa"/><w:tblLayout w:type="fixed"/>` +
        `<w:tblBorders><w:top w:val="single" w:sz="4" w:color="B8BEC7"/>` +
        `<w:left w:val="single" w:sz="4" w:color="B8BEC7"/>` +
        `<w:bottom w:val="single" w:sz="4" w:color="B8BEC7"/>` +
        `<w:right w:val="single" w:sz="4" w:color="B8BEC7"/>` +
        `<w:insideH w:val="single" w:sz="4" w:color="D5D9DF"/>` +
        `<w:insideV w:val="single" w:sz="4" w:color="D5D9DF"/></w:tblBorders>` +
        `<w:tblCellMar><w:top w:w="90" w:type="dxa"/><w:left w:w="120" w:type="dxa"/>` +
        `<w:bottom w:w="90" w:type="dxa"/><w:right w:w="120" w:type="dxa"/></w:tblCellMar>` +
        `</w:tblPr><w:tblGrid>${widths.map((width) => `<w:gridCol w:w="${width}"/>`).join("")}</w:tblGrid>`;
      rows.forEach((row, rowIndex) => {
        const isHeaderRow = rowIndex === 0 || [...row.children].every((cell) =>
          cell.tagName.toLowerCase() === "th");
        xml += `<w:tr>${isHeaderRow ? "<w:trPr><w:tblHeader/></w:trPr>" : ""}`;
        for (let column = 0; column < colCount; column += 1) {
          const cell = row.children[column];
          const header = isHeaderRow || cell?.tagName.toLowerCase() === "th";
          const cellRuns = cell ? runsFor(cell, header ? { bold: true } : {}) : [];
          xml += `<w:tc><w:tcPr><w:tcW w:w="${widths[column]}" w:type="dxa"/>` +
            `${header ? '<w:shd w:val="clear" w:fill="E9EDF2"/>' : ""}</w:tcPr>` +
            `${paragraph(cellRuns, { style: "TableText" })}</w:tc>`;
        }
        xml += "</w:tr>";
      });
      xml += "</w:tbl>";
      blocks.push(xml);
    };

    const renderBlock = (element, forcedStyle = "") => {
      const tag = element.tagName.toLowerCase();
      if (/^h[1-6]$/.test(tag)) {
        blocks.push(paragraph(runsFor(element), { style: `Heading${tag[1]}` }));
      } else if (tag === "p" || tag === "div") {
        blocks.push(paragraph(runsFor(element), forcedStyle ? { style: forcedStyle } : {}));
      } else if (tag === "pre") {
        const lines = docxSafeText(element.textContent).replaceAll("\r\n", "\n").split("\n");
        const runs = [];
        lines.forEach((line, index) => {
          if (index) runs.push({ break: true });
          runs.push({ text: line, code: true });
        });
        blocks.push(paragraph(runs, { style: "Code" }));
      } else if (tag === "blockquote") {
        if (element.children.length) {
          [...element.children].forEach((child) => renderBlock(child, "Quote"));
        } else {
          blocks.push(paragraph(runsFor(element), { style: "Quote" }));
        }
      } else if (tag === "ul" || tag === "ol") {
        renderList(element);
      } else if (tag === "table") {
        renderTable(element);
      } else if (tag === "hr") {
        blocks.push(paragraph([], { horizontalRule: true }));
      } else {
        blocks.push(paragraph(runsFor(element), forcedStyle ? { style: forcedStyle } : {}));
      }
    };

    [...previewEditor.children].forEach((element) => renderBlock(element));
    if (!blocks.length) blocks.push(paragraph([]));
    const orientation = settings.orientation === "landscape" ? ' w:orient="landscape"' : "";
    const headerFooter = Math.min(page.margin, 567);
    const section = `<w:sectPr><w:pgSz w:w="${page.width}" w:h="${page.height}"${orientation}/>` +
      `<w:pgMar w:top="${page.margin}" w:right="${page.margin}" w:bottom="${page.margin}" ` +
      `w:left="${page.margin}" w:header="${headerFooter}" w:footer="${headerFooter}" w:gutter="0"/>` +
      `<w:cols w:space="720"/><w:docGrid w:linePitch="360"/></w:sectPr>`;
    const namespaces = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" ' +
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' +
      'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" ' +
      'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ' +
      'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"';
    return {
      documentXml: `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>` +
        `<w:document ${namespaces}><w:body>${blocks.join("")}${section}</w:body></w:document>`,
      hyperlinks: context.hyperlinks,
      lists: context.lists
    };
  }

  async function buildDocxExportPayload() {
    synchronizeDocumentForPdf();
    await waitForPdfResources();
    const settings = docxSettingsFromControls();
    localStorage.setItem("mdviewer.docxSettings", JSON.stringify({
      paper: settings.paper,
      orientation: settings.orientation,
      marginMm: settings.marginMm,
      font: settings.font,
      includeImages: settings.includeImages,
      author: settings.author
    }));
    const prepared = await prepareDocumentImages(settings);
    const built = buildDocxDocument(settings, prepared.imageMap);
    return {
      ...settings,
      documentXml: built.documentXml,
      images: prepared.records.join("\n"),
      hyperlinks: built.hyperlinks.map((link) => `${link.id}\t${link.target}`).join("\n"),
      lists: built.lists.map((list) =>
        `${list.ordered ? "ordered" : "bullet"}\t${list.start}`).join("\n"),
      skippedImages: prepared.skipped
    };
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
      .forEach((item) => {
        item.hidden = !state.googleDriveAvailable;
        item.disabled = state.googleDriveBusy;
      });
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
    if (name === "file.print") {
      openPrintDialog();
    } else if (name === "file.export") {
      openExportDialog();
    } else if (name === "file.saveGoogleDriveAs") {
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

    const pdfExportDialog = $("#pdf-export-dialog");
    $$('[data-export-format-select]').forEach((select) =>
      select.addEventListener("change", () => switchExportFormat(select.value)));
    [$("#pdf-paper-select"), $("#pdf-margin-select"),
      $("#pdf-page-numbers"), $("#pdf-print-background"),
      ...$$('input[name="pdf-orientation"]')]
      .forEach((control) => control.addEventListener("change", () => {
        updatePdfPaperSummary();
        schedulePdfPreview();
      }));
    $$('input[name="pdf-print-pages"]').forEach((control) =>
      control.addEventListener("change", () => {
        updatePrintPageRangeControls();
        updatePdfPaperSummary();
        if (!$("#pdf-page-range-input").disabled) {
          $("#pdf-page-range-input").focus();
        }
        schedulePdfPreview();
      }));
    $("#pdf-page-range-input").addEventListener("input", () => {
      updatePrintPageRangeControls();
      updatePdfPaperSummary();
      schedulePdfPreview(320);
    });
    $$('[data-pdf-export-cancel]').forEach((button) =>
      button.addEventListener("click", closePdfExportDialog));
    pdfExportDialog.addEventListener("cancel", (event) => {
      event.preventDefault();
      closePdfExportDialog();
    });
    $("#pdf-preview-frame").addEventListener("load", () => {
      pdfPreviewFrameLoaded = true;
      if (pdfDialogMode === "print" && pdfExportDialog.open &&
          !$("#pdf-preview-frame").hidden) {
        updatePdfActionAvailability();
        $("#pdf-export-status").textContent = i18n.t("Preview ready");
      }
    });
    $("#pdf-printer-select").addEventListener("change", () => {
      preferredPrinterName = $("#pdf-printer-select").value;
      storePrintSettings();
      updatePrinterPropertiesAvailability();
      updatePdfActionAvailability();
    });
    $("#pdf-printer-properties").addEventListener("click", () => {
      const printerName = $("#pdf-printer-select").value;
      if (!printerName || pdfPrinterPropertiesBusy || pdfPrintBusy) return;
      setPrinterPropertiesBusy(true);
      if (!post("printer.properties", { printerName })) {
        setPrinterPropertiesBusy(false);
        showPrinterPropertiesStatus(
          i18n.t("Printer settings could not be opened"), "error");
      }
    });
    $("#pdf-print-copies").addEventListener("input", () => {
      storePrintSettings();
      updatePdfActionAvailability();
    });
    $("#pdf-preview-retry").addEventListener("click", () => schedulePdfPreview(0));
    $("#pdf-export-save").addEventListener("click", () => {
      if (!activePdfPreviewRequest) return;
      $("#pdf-export-save").disabled = true;
      const printing = pdfDialogMode === "print";
      $("#pdf-export-status").textContent = i18n.t(
        printing ? "Sending to printer…" : "Saving PDF…");
      const device = printDeviceSettingsFromControls();
      if (printing) setPdfPrintBusy(true);
      if (!post(printing ? "pdf.print" : "pdf.save", printing
        ? {
          requestId: activePdfPreviewRequest,
          printerName: device.printerName,
          copies: device.copies
        } : { requestId: activePdfPreviewRequest })) {
        if (printing) setPdfPrintBusy(false);
        showPdfPreviewState("error");
      }
    });

    const docxExportDialog = $("#docx-export-dialog");
    [$("#docx-paper-select"), $("#docx-margin-select"),
      $("#docx-font-select"), $("#docx-include-images"),
      ...$$("input[name='docx-orientation']")]
      .forEach((control) => control.addEventListener("change", () => {
        updateDocxSummary();
        updateDocxContentPreview();
      }));
    $$('[data-docx-export-cancel]').forEach((button) =>
      button.addEventListener("click", closeDocxExportDialog));
    docxExportDialog.addEventListener("cancel", (event) => {
      event.preventDefault();
      closeDocxExportDialog();
    });
    $("#docx-export-save").addEventListener("click", async () => {
      if (docxExportBusy) return;
      docxExportBusy = true;
      $("#docx-export-save").disabled = true;
      $("#docx-export-status").textContent = i18n.t("Preparing DOCX…");
      try {
        const payload = await buildDocxExportPayload();
        $("#docx-export-status").textContent = payload.skippedImages
          ? i18n.t("Some images could not be embedded: {count}",
            { count: payload.skippedImages })
          : i18n.t("Saving DOCX…");
        if (!post("docx.export", payload)) throw new Error("host unavailable");
      } catch (error) {
        console.error(error);
        docxExportBusy = false;
        $("#docx-export-save").disabled = false;
        $("#docx-export-status").textContent = i18n.t("DOCX could not be saved");
      }
    });

    const hwpxExportDialog = $("#hwpx-export-dialog");
    [$("#hwpx-paper-select"), $("#hwpx-margin-select"),
      $("#hwpx-font-select"), $("#hwpx-include-images"),
      ...$$("input[name='hwpx-orientation']")]
      .forEach((control) => control.addEventListener("change", () => {
        updateHwpxSummary();
        updateHwpxContentPreview();
      }));
    $$('[data-hwpx-export-cancel]').forEach((button) =>
      button.addEventListener("click", closeHwpxExportDialog));
    hwpxExportDialog.addEventListener("cancel", (event) => {
      event.preventDefault();
      closeHwpxExportDialog();
    });
    $("#hwpx-export-save").addEventListener("click", async () => {
      if (hwpxExportBusy) return;
      hwpxExportBusy = true;
      $("#hwpx-export-save").disabled = true;
      $("#hwpx-export-status").textContent = i18n.t("Preparing HWPX…");
      try {
        const payload = await buildHwpxExportPayload();
        $("#hwpx-export-status").textContent = payload.skippedImages
          ? i18n.t("Some images could not be embedded: {count}",
            { count: payload.skippedImages })
          : i18n.t("Saving HWPX…");
        if (!post("hwpx.export", payload)) throw new Error("host unavailable");
      } catch (error) {
        console.error(error);
        hwpxExportBusy = false;
        $("#hwpx-export-save").disabled = false;
        $("#hwpx-export-status").textContent = i18n.t("HWPX could not be saved");
      }
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
      } else if (key === "e" && event.shiftKey) {
        event.preventDefault();
        openExportDialog();
      } else if (key === "p" && !event.shiftKey) {
        event.preventDefault();
        openPrintDialog();
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
        if (message.capabilities) {
          state.googleDriveAvailable = message.capabilities.googleDrive !== false;
        }
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
      } else if (message.type === "pdf.previewReady") {
        if ($("#pdf-export-dialog").open &&
            Number(message.requestId) === activePdfPreviewRequest) {
          pdfPreviewFrameLoaded = false;
          $("#pdf-preview-frame").src = `${message.url}#toolbar=0&navpanes=0`;
          showPdfPreviewState("ready");
        }
      } else if (message.type === "pdf.previewFailed") {
        if ($("#pdf-export-dialog").open &&
            Number(message.requestId) === activePdfPreviewRequest) {
          showPdfPreviewState("error");
        }
      } else if (message.type === "pdf.saved") {
        const path = message.path || "";
        closePdfExportDialog();
        showToast(i18n.t("PDF saved to {path}", { path }), "success");
      } else if (message.type === "pdf.saveCanceled") {
        showPdfPreviewState("ready");
      } else if (message.type === "pdf.saveFailed") {
        $("#pdf-export-save").disabled = false;
        $("#pdf-export-status").textContent = i18n.t("PDF could not be saved");
      } else if (message.type === "printer.listed") {
        applyPrinterList(message.printers);
      } else if (message.type === "printer.propertiesStarted") {
        setPrinterPropertiesBusy(true);
      } else if (message.type === "printer.propertiesApplied") {
        advancedSettingsPrinterName = message.printerName ||
          $("#pdf-printer-select").value;
        setPrinterPropertiesBusy(false);
        updatePrinterPropertiesAvailability();
      } else if (message.type === "printer.propertiesCanceled") {
        setPrinterPropertiesBusy(false);
        updatePrinterPropertiesAvailability();
      } else if (message.type === "printer.propertiesFailed") {
        setPrinterPropertiesBusy(false);
        showPrinterPropertiesStatus(message.message ||
          i18n.t("Printer settings could not be opened"), "error");
      } else if (message.type === "pdf.printStarted") {
        $("#pdf-export-status").textContent = i18n.t("Sending to printer…");
      } else if (message.type === "pdf.printed") {
        const printerName = message.printerName || preferredPrinterName;
        setPdfPrintBusy(false);
        closePdfExportDialog();
        showToast(i18n.t("Print job sent to {printer}", {
          printer: printerName
        }), "success");
      } else if (message.type === "pdf.printFailed") {
        setPdfPrintBusy(false);
        $("#pdf-export-status").textContent = message.message ||
          i18n.t("Printing could not be started");
      } else if (message.type === "docx.saved") {
        const path = message.path || "";
        docxExportBusy = false;
        closeDocxExportDialog();
        showToast(i18n.t("DOCX saved to {path}", { path }), "success");
      } else if (message.type === "docx.saveCanceled") {
        docxExportBusy = false;
        $("#docx-export-save").disabled = false;
        $("#docx-export-status").textContent = i18n.t("Ready to export");
      } else if (message.type === "docx.saveFailed") {
        docxExportBusy = false;
        $("#docx-export-save").disabled = false;
        $("#docx-export-status").textContent = i18n.t("DOCX could not be saved");
      } else if (message.type === "hwpx.saved") {
        const path = message.path || "";
        hwpxExportBusy = false;
        closeHwpxExportDialog();
        showToast(i18n.t("HWPX saved to {path}", { path }), "success");
      } else if (message.type === "hwpx.saveCanceled") {
        hwpxExportBusy = false;
        $("#hwpx-export-save").disabled = false;
        $("#hwpx-export-status").textContent = i18n.t("Ready to export");
      } else if (message.type === "hwpx.saveFailed") {
        hwpxExportBusy = false;
        $("#hwpx-export-save").disabled = false;
        $("#hwpx-export-status").textContent = i18n.t("HWPX could not be saved");
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
      updatePdfPaperSummary();
      updateDocxSummary();
      updateHwpxSummary();
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
