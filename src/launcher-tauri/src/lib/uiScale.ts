import { createStore, useStore } from "../store/observable";

/** Whole-UI text/element scale (Settings > Appearance) -- a persisted user
 * preference, not an accessibility OS setting. Applied as CSS `zoom` on the
 * document root (main.tsx), not `transform: scale` or a rem-wide refactor:
 * `zoom` is a real WebKit property (confirmed live in this exact WebKitGTK
 * build, not assumed) that reflows layout and hit-testing at the scaled
 * size, so focus rings, click targets and FocusNav's own getBoundingClientRect
 * math all stay correct at every scale -- transform:scale only repaints
 * pixels, it does not affect layout, and would desync click targets from
 * what's drawn. */
export type UiScale = "90" | "100" | "115" | "130";

const STORAGE_KEY = "kyty.uiScale";
const DEFAULT_SCALE: UiScale = "100";
const VALID: readonly UiScale[] = ["90", "100", "115", "130"];

function readStored(): UiScale {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return (VALID as readonly string[]).includes(raw ?? "") ? (raw as UiScale) : DEFAULT_SCALE;
  } catch {
    return DEFAULT_SCALE;
  }
}

const uiScaleStore = createStore<UiScale>(readStored());

export function useUiScale(): UiScale {
  return useStore(uiScaleStore);
}

function apply(scale: UiScale): void {
  document.documentElement.style.setProperty("--ui-scale", (Number(scale) / 100).toString());
}

export function setUiScale(scale: UiScale): void {
  uiScaleStore.set(scale);
  try {
    localStorage.setItem(STORAGE_KEY, scale);
  } catch {
    // Best-effort persistence only -- same posture as theme.ts/profiles.ts.
  }
  apply(scale);
}

/** Call once at boot (App.tsx) to apply whatever scale was last stored (or
 * the 100% default) before the first paint settles. */
export function applyStoredUiScaleAtBoot(): void {
  apply(uiScaleStore.get());
}
