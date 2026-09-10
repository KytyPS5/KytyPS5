import { getCurrentWindow } from "@tauri-apps/api/window";
import { createStore, useStore } from "../store/observable";

/** Whether the launcher starts fullscreen or windowed -- a persisted user
 * preference (Settings > Appearance), independent of tauri.conf.json's own
 * static window config and of TopBar's manual fullscreen toggle (that one
 * changes the live window without touching this stored preference; this
 * setting only governs what state the window boots into). */
export type DisplayMode = "full" | "window";

const STORAGE_KEY = "kyty.displayMode";
const DEFAULT_MODE: DisplayMode = "full";

function readStored(): DisplayMode {
  try {
    return localStorage.getItem(STORAGE_KEY) === "window" ? "window" : DEFAULT_MODE;
  } catch {
    return DEFAULT_MODE;
  }
}

const displayModeStore = createStore<DisplayMode>(readStored());

export function useDisplayMode(): DisplayMode {
  return useStore(displayModeStore);
}

export function setDisplayMode(mode: DisplayMode): void {
  displayModeStore.set(mode);
  try {
    localStorage.setItem(STORAGE_KEY, mode);
  } catch {
    // Best-effort persistence only -- same posture as theme.ts/profiles.ts.
  }
  void getCurrentWindow().setFullscreen(mode === "full");
}

/** Call once at boot (App.tsx). tauri.conf.json's window always starts
 * non-fullscreen, so this is what actually promotes it to fullscreen for
 * the default (and any stored "full") preference. */
export function applyStoredDisplayModeAtBoot(): void {
  void getCurrentWindow().setFullscreen(displayModeStore.get() === "full");
}
