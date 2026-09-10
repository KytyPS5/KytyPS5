// Deliberately no i18n library -- createStore/useSyncExternalStore
// (store/observable.ts) is already exactly the shape an active-locale store
// needs. Locale catalogs are dynamically imported so only the active
// language ships at runtime; English is bundled directly since it is also
// the fallback for any key a translated catalog is missing.

import { createStore, useStore } from "../store/observable";
import { DEFAULT_LANGUAGE_CODE, LANGUAGES, type Language } from "./languages";
import en, { type Catalog, type DeepPartial } from "./locales/en";

const STORAGE_KEY = "kyty.locale";

type LocaleModule = { default: DeepPartial<Catalog> };

// One loader per non-English language. Adding a locale after this wiring
// lands is a pure content change (a new locales/<code>.ts file) plus one
// line here -- no other code changes.
const loaders: Record<string, () => Promise<LocaleModule>> = {
  "zh-Hans": () => import("./locales/zh-Hans"),
  "zh-Hant": () => import("./locales/zh-Hant"),
  es: () => import("./locales/es"),
  "pt-BR": () => import("./locales/pt-BR"),
  ru: () => import("./locales/ru"),
  ja: () => import("./locales/ja"),
  de: () => import("./locales/de"),
  fr: () => import("./locales/fr"),
  ko: () => import("./locales/ko"),
  it: () => import("./locales/it"),
  tr: () => import("./locales/tr"),
  vi: () => import("./locales/vi"),
  pl: () => import("./locales/pl"),
  nl: () => import("./locales/nl"),
  th: () => import("./locales/th"),
  id: () => import("./locales/id"),
  ar: () => import("./locales/ar"),
};

function readStoredCode(): string {
  try {
    const v = localStorage.getItem(STORAGE_KEY);
    return v && LANGUAGES.some((l) => l.code === v) ? v : DEFAULT_LANGUAGE_CODE;
  } catch {
    // Private-window/blocked-storage fallback -- never let a locale read
    // crash the app, just don't persist it.
    return DEFAULT_LANGUAGE_CODE;
  }
}

function languageFor(code: string): Language {
  return LANGUAGES.find((l) => l.code === code) ?? LANGUAGES[0];
}

/** Recursively fills any key a translated catalog is missing (or hasn't
 * caught up on yet) with the English value, so a partially-translated
 * locale file never renders `undefined` or a raw key. */
function mergeWithFallback<T>(base: T, partial: DeepPartial<T>): T {
  if (typeof base !== "object" || base === null) return (partial ?? base) as T;
  const out: any = Array.isArray(base) ? [...base] : { ...base };
  for (const key of Object.keys(base as object)) {
    const partialValue = (partial as any)?.[key];
    if (partialValue === undefined) continue;
    out[key] = mergeWithFallback((base as any)[key], partialValue);
  }
  return out as T;
}

const localeStore = createStore<string>(readStoredCode());
const catalogStore = createStore<Catalog>(en);

function applyDocumentAttrs(lang: Language): void {
  document.documentElement.lang = lang.code;
  document.documentElement.dir = lang.rtl ? "rtl" : "ltr";
}

/** Switches the active UI language, persists the choice, and loads (and
 * caches) that locale's catalog. Safe to call before the catalog has
 * finished loading -- components keep reading the previous catalog (or
 * English) until it resolves. */
export function setLocale(code: string): void {
  const lang = languageFor(code);
  localeStore.set(lang.code);
  try {
    localStorage.setItem(STORAGE_KEY, lang.code);
  } catch {
    // Best-effort persistence only -- see readStoredCode's catch.
  }
  applyDocumentAttrs(lang);

  if (lang.code === DEFAULT_LANGUAGE_CODE) {
    catalogStore.set(en);
    return;
  }
  const load = loaders[lang.code];
  if (!load) return;
  void load().then((mod) => {
    // Bail if the user switched languages again while this was loading.
    if (localeStore.get() !== lang.code) return;
    catalogStore.set(mergeWithFallback(en, mod.default));
  });
}

/** Call once at boot (after loadPrefs-style init) to load whatever locale
 * was last persisted -- readStoredCode already seeded the stores'  initial
 * values synchronously, so this just kicks off the async catalog load. */
export function initLocale(): void {
  const code = localeStore.get();
  applyDocumentAttrs(languageFor(code));
  if (code !== DEFAULT_LANGUAGE_CODE) setLocale(code);
}

export function useLocale(): Language {
  const code = useStore(localeStore);
  return languageFor(code);
}

/** Non-hook read of the active locale code -- for use outside components
 * (e.g. lib/profiles.ts seeding a new profile from the current locale). */
export function getLocaleCode(): string {
  return localeStore.get();
}

function resolve(catalog: Catalog, key: string): string | undefined {
  const parts = key.split(".");
  let node: unknown = catalog;
  for (const part of parts) {
    if (node == null || typeof node !== "object") return undefined;
    node = (node as Record<string, unknown>)[part];
  }
  return typeof node === "string" ? node : undefined;
}

/** Dot-path key lookup (e.g. "settings.gamepad.deadzone") with `{name}`-style
 * interpolation and an English fallback for any key the active catalog is
 * missing. Not a hook -- safe to call from plain functions (formatPlaytime)
 * as well as components. */
export function t(key: string, vars?: Record<string, string | number>): string {
  const catalog = catalogStore.get();
  let str = resolve(catalog, key) ?? resolve(en, key) ?? key;
  if (vars) {
    for (const [name, value] of Object.entries(vars)) {
      str = str.replaceAll(`{${name}}`, String(value));
    }
  }
  return str;
}

/** Subscribes the calling component to locale changes and returns `t` bound
 * to the now-active catalog. */
export function useT(): typeof t {
  useStore(catalogStore);
  return t;
}

/** The PS5 console-language display names, translated -- index-matched to
 * languages.ts's consoleLanguageId / emulator.rs's --console-language.
 * Kept out of the generic t() path since it's an array, not a string. */
export function useConsoleLanguageNames(): string[] {
  const catalog = useStore(catalogStore);
  return catalog.configForm.consoleLanguages;
}

export { LANGUAGES } from "./languages";
export type { Language } from "./languages";
export type { Catalog } from "./locales/en";
