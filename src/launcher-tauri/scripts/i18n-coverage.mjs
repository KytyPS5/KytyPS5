// Reports each translated locale's key coverage against en.ts, the
// source-of-truth catalog. A missing key isn't a build error (mergeWithFallback
// in src/i18n/index.ts deliberately falls back to English for it) but it's a
// silent i18n gap otherwise, so this script is how the gap gets seen.
//
// Usage: node scripts/i18n-coverage.mjs
// Exit code is non-zero when any locale is missing or has stale keys.

import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const localesDir = path.join(__dirname, "..", "src", "i18n", "locales");

// Locale files are plain TS object literals with a type annotation and an
// `import type ...` line. Strip both, then execute the remaining object
// literal as JS via a data: URL import -- no ts-node/build step needed, and
// it reads real evaluated values (multi-line strings, escaped quotes,
// template-literal-free interpolation placeholders) rather than guessing at
// them with a regex.
function stripInterfaceBlock(src, name) {
  const start = src.indexOf(`interface ${name}`);
  if (start === -1) return src;
  let depth = 0;
  let i = src.indexOf("{", start);
  const braceStart = i;
  for (; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}") {
      depth--;
      if (depth === 0) break;
    }
  }
  return src.slice(0, src.lastIndexOf("export", start)) + src.slice(i + 1);
}

function loadCatalog(file) {
  // Normalised to LF first. Every strip below is a regex anchored on a bare
  // newline, and git checks these files out with CRLF on Windows
  // (core.autocrlf), so on a Windows clone the DeepPartial strip matched
  // nothing: the `export type` survived into the data: URL and the whole
  // script died with "SyntaxError: Unexpected token 'export'". It reports
  // coverage fine on Linux, which is why this went unnoticed.
  const src = readFileSync(path.join(localesDir, file), "utf8").replace(/\r\n/g, "\n");
  const stripped = stripInterfaceBlock(src, "Catalog")
    .replace(/export type DeepPartial<T>[\s\S]*?:\s*T;\n/, "")
    .replace(/^import[^\n]*\n/gm, "")
    .replace(/:\s*Catalog\b/g, "")
    .replace(/:\s*DeepPartial<Catalog>/g, "");
  const dataUrl = "data:text/javascript," + encodeURIComponent(stripped);
  return import(dataUrl).then((mod) => mod.default);
}

function flatten(obj, prefix = "") {
  const out = {};
  for (const [key, value] of Object.entries(obj)) {
    const path = prefix ? `${prefix}.${key}` : key;
    if (value !== null && typeof value === "object" && !Array.isArray(value)) {
      Object.assign(out, flatten(value, path));
    } else {
      out[path] = value;
    }
  }
  return out;
}

const files = readdirSync(localesDir).filter((f) => f.endsWith(".ts"));
if (!files.includes("en.ts")) {
  console.error("en.ts not found in", localesDir);
  process.exit(1);
}

const en = flatten(await loadCatalog("en.ts"));
const enKeys = Object.keys(en);
const enOrder = new Map(enKeys.map((k, i) => [k, i]));

let failed = false;

for (const file of files.filter((f) => f !== "en.ts").sort()) {
  const locale = flatten(await loadCatalog(file));
  const localeKeys = new Set(Object.keys(locale));

  const missing = enKeys.filter((k) => !localeKeys.has(k));
  const stale = Object.keys(locale)
    .filter((k) => !(k in en))
    .sort();

  const consoleLangEn = en["configForm.consoleLanguages"];
  const consoleLangLocale = locale["configForm.consoleLanguages"];
  const arrayMismatch =
    Array.isArray(consoleLangEn) &&
    Array.isArray(consoleLangLocale) &&
    consoleLangEn.length !== consoleLangLocale.length;

  const ok = missing.length === 0 && stale.length === 0 && !arrayMismatch;
  console.log(`\n${file}: ${ok ? "OK" : "GAPS"} (${localeKeys.size}/${enKeys.length} keys)`);

  if (missing.length > 0) {
    failed = true;
    console.log(`  missing (${missing.length}):`);
    for (const k of missing.sort((a, b) => enOrder.get(a) - enOrder.get(b))) {
      console.log(`    ${k}`);
    }
  }
  if (stale.length > 0) {
    failed = true;
    console.log(`  stale (${stale.length}, not in en.ts):`);
    for (const k of stale) console.log(`    ${k}`);
  }
  if (arrayMismatch) {
    failed = true;
    console.log(
      `  configForm.consoleLanguages length mismatch: en has ${consoleLangEn.length}, ${file} has ${consoleLangLocale.length}`,
    );
  }
}

console.log(`\n${failed ? "FAIL" : "PASS"}: ${files.length - 1} locales checked against en.ts (${enKeys.length} keys)`);
process.exit(failed ? 1 : 0);
