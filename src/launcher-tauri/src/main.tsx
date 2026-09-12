import React from "react";
import ReactDOM from "react-dom/client";
// Self-hosted, license-clean font (SIL OFL). Bundled, never fetched, the
// launcher must not depend on network access to render its own text.
// Inter carries every role (display, UI chrome, body) -- see theme.css's font
// tokens for why, this is the PS5 reference's own humanist-grotesque register.
import "@fontsource-variable/inter/wght.css";
// Imported BEFORE App on purpose (owner intent, theme.css:541-544's own
// comment on [data-focusable]/.ps-focused): a component's CSS-module rule
// should win an equal-specificity tie against this file's global one. CSS
// module chunks land in the bundle in import order, so theme.css must be
// evaluated first -- component modules (GameTile.module.css etc.), pulled
// in transitively while resolving `import App from "./App"` below, then
// load after it and win ties as intended. Importing App first (as this
// file previously did) put every component module before theme.css
// instead, so [data-focusable]'s generic transition list silently beat
// .tile's/.art's own -- confirmed: opacity/filter were not transitioning
// at all on tiles/cards, and transform ran at 120ms instead of the
// intended 220ms, both because theme.css was the one winning the tie.
import "./theme.css";
import App from "./App";
import { runPerfProbe } from "./lib/perf";
import { installIdleThrottle } from "./lib/idle";
import { installContextMenuSuppression } from "./lib/contextMenu";
import { initLocale } from "./i18n";

initLocale();

try {
  ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
    <React.StrictMode>
      <App />
    </React.StrictMode>,
  );
} catch (e) {
  const el = document.getElementById("boot-error");
  if (el) {
    el.style.display = "block";
    el.textContent += `render threw: ${e instanceof Error ? (e.stack ?? e.message) : String(e)}\n`;
  }
  throw e;
}

runPerfProbe();
installIdleThrottle();
installContextMenuSuppression();
