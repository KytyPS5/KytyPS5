import { convertFileSrc } from "@tauri-apps/api/core";
import { createStore, useStore } from "../store/observable";

/** Dashboard dynamic themes -- fixed animated backgrounds the user picks in
 * Settings. The 2026-09-08 decision recorded here previously made this the
 * Home background outright, replacing the per-game art crossfade to match
 * the real PS5's own "background does not follow selection" behavior; the
 * PS5 clone pass (2026-09-09) reversed that call, since matching the
 * reference's home screen more literally means the background DOES follow
 * the focused game after all (Home.tsx's activeGame branch). A theme here is
 * now the fallback only: no active game (empty library), a game whose art
 * hasn't resolved, or a game whose art 404s -- and it stays the fixed
 * backdrop for Settings, Library and Profile, which never show game art. */
export interface DashboardTheme {
  id: string;
  label: string;
  url: string;
  /** Tints the HeroBackground accent radial glow when this theme is the one
   * actually showing. On Home with an active game, superseded by that
   * game's own art-derived accent (lib/accent.ts); the Settings thumbnail
   * selection ring uses the app's single --accent token instead, not this
   * per-theme value. */
  accent: string;
}

export const CUSTOM_THEME_ID = "custom";

export const THEMES: DashboardTheme[] = [
  { id: "nebula", label: "Nebula Drift", url: "/art/themes/theme_nebula.jpg", accent: "#7c5cff" },
  { id: "particles", label: "Particle Field", url: "/art/themes/theme_particles.jpg", accent: "#00d4ff" },
  { id: "waves", label: "Geometric Waves", url: "/art/themes/theme_waves.jpg", accent: "#0070d1" },
  { id: "aurora", label: "Aurora Glow", url: "/art/themes/theme_aurora.jpg", accent: "#00e6c8" },
  // The art (public/art/themes/theme_deepspace.jpg) and every locale's
  // "Deep Space" label (i18n/locales/*.ts's settings.background.themes.
  // deepspace) already existed; only this entry was missing.
  { id: "deepspace", label: "Deep Space", url: "/art/themes/theme_deepspace.jpg", accent: "#4a5ce0" },
];

const STORAGE_KEY = "kyty.dashboardTheme";
const CUSTOM_PATH_KEY = "kyty.customBackgroundPath";

function readStored(): string {
  try {
    return localStorage.getItem(STORAGE_KEY) ?? THEMES[0].id;
  } catch {
    // Private-window/blocked-storage fallback -- never let a theme pick
    // crash the dashboard, just don't persist it.
    return THEMES[0].id;
  }
}

function readStoredCustomPath(): string | null {
  try {
    return localStorage.getItem(CUSTOM_PATH_KEY);
  } catch {
    return null;
  }
}

const themeIdStore = createStore<string>(readStored());
const customPathStore = createStore<string | null>(readStoredCustomPath());

/** The user's chosen local image for the "Custom" background slot
 * (Settings > Dashboard background), or null when none is set yet -- the
 * slot then shows a "+" and picking one goes through the same in-app
 * browse_folder command as game folders / per-game art, filtered to image
 * extensions (no native file-dialog plugin dependency, matching this
 * app's existing convention). */
export function useCustomBackgroundPath(): string | null {
  return useStore(customPathStore);
}

export function setCustomBackgroundPath(path: string | null): void {
  customPathStore.set(path);
  try {
    if (path) localStorage.setItem(CUSTOM_PATH_KEY, path);
    else localStorage.removeItem(CUSTOM_PATH_KEY);
  } catch {
    // Best-effort persistence only -- see readStored's catch.
  }
  // Removing the image out from under the active selection would otherwise
  // leave the dashboard pointed at a background that no longer resolves.
  if (!path && themeIdStore.get() === CUSTOM_THEME_ID) {
    setDashboardTheme(THEMES[0].id);
  }
}

export function useDashboardTheme(): DashboardTheme {
  const id = useStore(themeIdStore);
  const customPath = useStore(customPathStore);
  if (id === CUSTOM_THEME_ID && customPath) {
    return { id: CUSTOM_THEME_ID, label: "Custom", url: convertFileSrc(customPath), accent: THEMES[0].accent };
  }
  return THEMES.find((t) => t.id === id) ?? THEMES[0];
}

export function setDashboardTheme(id: string): void {
  themeIdStore.set(id);
  try {
    localStorage.setItem(STORAGE_KEY, id);
  } catch {
    // Best-effort persistence only -- see readStored's catch.
  }
}
