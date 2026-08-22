(() => {
  "use strict";

  const DEFAULT_LOCALE = "ko-KR";
  const SUPPORTED_LOCALES = Object.freeze({
    "en-US": { label: "English", shortLabel: "EN" },
    "ko-KR": { label: "한국어", shortLabel: "KO" },
    "ja-JP": { label: "日本語", shortLabel: "JA" },
    "fr-FR": { label: "Français", shortLabel: "FR" },
    "de-DE": { label: "Deutsch", shortLabel: "DE" },
    "ru-RU": { label: "Русский", shortLabel: "RU" },
    "zh-CN": { label: "简体中文", shortLabel: "简" },
    "zh-TW": { label: "繁體中文", shortLabel: "繁" },
    "es-ES": { label: "Español", shortLabel: "ES" },
    "pt-BR": { label: "Português (Brasil)", shortLabel: "PT" },
    "hi-IN": { label: "हिन्दी", shortLabel: "HI" },
    "id-ID": { label: "Bahasa Indonesia", shortLabel: "ID" }
  });

  let currentLocale = DEFAULT_LOCALE;
  let messages = Object.create(null);

  function normalizeLocale(locale) {
    const exact = String(locale || "").replaceAll("_", "-");
    if (SUPPORTED_LOCALES[exact]) return exact;
    const lower = exact.toLowerCase();
    if (lower.startsWith("zh-hant") || lower.startsWith("zh-tw") || lower.startsWith("zh-hk")) return "zh-TW";
    if (lower.startsWith("zh")) return "zh-CN";
    const mapping = { en: "en-US", ko: "ko-KR", ja: "ja-JP", fr: "fr-FR",
      de: "de-DE", ru: "ru-RU", es: "es-ES", pt: "pt-BR", hi: "hi-IN", id: "id-ID" };
    return mapping[lower.split("-")[0]] || DEFAULT_LOCALE;
  }

  async function loadMessages(locale) {
    if (locale === "en-US") return Object.create(null);
    const response = await fetch(`locales/${locale}.json`, { cache: "no-store" });
    if (!response.ok) throw new Error(`Could not load locale '${locale}' (${response.status}).`);
    return response.json();
  }

  function interpolate(template, parameters) {
    return String(template).replace(/\{([A-Za-z][A-Za-z0-9]*)\}/g,
      (match, name) => Object.prototype.hasOwnProperty.call(parameters, name)
        ? String(parameters[name]) : match);
  }

  function t(message, parameters = {}) {
    return interpolate(messages[message] ?? message, parameters);
  }

  function localizeDocument(root = document) {
    root.querySelectorAll("[data-i18n]").forEach((element) => {
      let text = t(element.dataset.i18n);
      if (element.hasAttribute("data-menu-title")) {
        text = text.replace(/\(&[A-Za-z]\)/g, "").replace(/&(?=\p{L})/u, "");
      } else if (element.closest('[role="menuitem"], [role="menuitemradio"], [role="menuitemcheckbox"]')) {
        text = text.replace(/\(&[A-Za-z]\)/g, "").replace(/&(?=\p{L})/u, "");
      }
      element.textContent = text;
    });
    for (const attribute of ["title", "aria-label", "placeholder"]) {
      const dataName = `i18n${attribute.split("-")
        .map((part) => part[0].toUpperCase() + part.slice(1)).join("")}`;
      root.querySelectorAll(`[data-i18n-${attribute}]`).forEach((element) => {
        element.setAttribute(attribute, t(element.dataset[dataName]));
      });
    }
  }

  async function setLocale(locale) {
    const normalized = normalizeLocale(locale);
    try {
      messages = await loadMessages(normalized);
      currentLocale = normalized;
    } catch (error) {
      console.error(error);
      messages = Object.create(null);
      currentLocale = "en-US";
    }
    document.documentElement.lang = currentLocale;
    localStorage.setItem("mdviewer.language", currentLocale);
    localizeDocument();
    window.dispatchEvent(new CustomEvent("mdviewerlanguageapplied", { detail: currentLocale }));
    return currentLocale;
  }

  async function initialize() {
    const queryLocale = new URLSearchParams(location.search).get("lang");
    const storedLocale = localStorage.getItem("mdviewer.language");
    return setLocale(queryLocale || storedLocale || DEFAULT_LOCALE);
  }

  window.MdViewerI18n = Object.freeze({
    initialize, localizeDocument, normalizeLocale, setLocale, t,
    get locale() { return currentLocale; },
    get supportedLocales() { return SUPPORTED_LOCALES; }
  });
})();
