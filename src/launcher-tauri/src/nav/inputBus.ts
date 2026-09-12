import { listen } from "@tauri-apps/api/event";

/** Frontend half of the always-on gamepad-to-navigation pipeline (see
 * src-tauri/src/gamepad.rs's module doc). That Rust thread already applies
 * deadzone, direction resolution and repeat timing; this module's only job
 * is to fan the resulting Tauri events out to subscribers synchronously --
 * no React state in between, so nothing here can collapse two events fired
 * in the same tick into one the way the old useState-based delivery did
 * (see useGamepadActions.ts's previous doc comment, kept in git history).
 *
 * This is the navigation path. It is deliberately separate from
 * nav/gamepadSource.ts (raw per-frame polling, kept alive solely for
 * InputMappingDialog's remap-capture overlay, which needs to know *which*
 * physical button/axis just moved -- a question this semantic
 * up/down/confirm stream cannot answer). Nothing here polls anything: it
 * only listens. */

export interface NavIntent {
  action: "up" | "down" | "left" | "right" | "confirm" | "back" | "menu" | "pageUp" | "pageDown";
  seq: number;
  source: "button" | "stick";
  magnitude: number;
}

export interface NavScroll {
  x: number;
  y: number;
}

type IntentListener = (intent: NavIntent) => void;
type ScrollListener = (scroll: NavScroll) => void;
type PadsListener = (names: string[]) => void;

const intentListeners = new Set<IntentListener>();
const scrollListeners = new Set<ScrollListener>();
const padsListeners = new Set<PadsListener>();

let started = false;
/** Most recent connected-pad names, for a late subscriber (e.g. the
 * diagnostics overlay mounting after boot) to read synchronously instead of
 * waiting for the next connect/disconnect edge. */
let lastPads: string[] = [];

function ensureStarted(): void {
  if (started) return;
  started = true;
  void listen<NavIntent>("nav-intent", (event) => {
    for (const l of intentListeners) l(event.payload);
  });
  void listen<NavScroll>("nav-scroll", (event) => {
    for (const l of scrollListeners) l(event.payload);
  });
  void listen<{ names: string[] }>("nav-pads", (event) => {
    lastPads = event.payload.names;
    for (const l of padsListeners) l(lastPads);
  });
}

export function subscribeNavIntent(listener: IntentListener): () => void {
  ensureStarted();
  intentListeners.add(listener);
  return () => intentListeners.delete(listener);
}

export function subscribeNavScroll(listener: ScrollListener): () => void {
  ensureStarted();
  scrollListeners.add(listener);
  return () => scrollListeners.delete(listener);
}

export function subscribeNavPads(listener: PadsListener): () => void {
  ensureStarted();
  padsListeners.add(listener);
  listener(lastPads);
  return () => padsListeners.delete(listener);
}
