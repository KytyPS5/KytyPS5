import { invoke } from "@tauri-apps/api/core";
import type { GameEntry, KytyConfig, LauncherPrefs } from "../types";
import { createStore } from "./observable";

export const configStore = createStore<KytyConfig | null>(null);
export const gamesStore = createStore<GameEntry[]>([]);
export const prefsStore = createStore<LauncherPrefs | null>(null);
export const libraryLoadingStore = createStore(false);
export const libraryErrorStore = createStore<string | null>(null);

/** Reloads settings, rescans every game folder, and reloads prefs — call
 * after any change that could affect the library (settings save, folder
 * add/remove) and once at startup. */
export async function refreshLibrary(): Promise<void> {
  libraryLoadingStore.set(true);
  libraryErrorStore.set(null);
  try {
    const [cfg, games] = await Promise.all([
      invoke<KytyConfig>("load_config"),
      invoke<GameEntry[]>("scan_games"),
    ]);
    configStore.set(cfg);
    gamesStore.set(games);
  } catch (e) {
    libraryErrorStore.set(e instanceof Error ? e.message : String(e));
  } finally {
    libraryLoadingStore.set(false);
  }
}

export async function loadPrefs(): Promise<void> {
  prefsStore.set(await invoke<LauncherPrefs>("get_prefs"));
}

export async function savePrefs(next: LauncherPrefs): Promise<void> {
  await invoke("save_prefs", { prefs: next });
  prefsStore.set(next);
}

/** Persists the given config to Kyty.ini, then rescans so the game list
 * reflects the change immediately. */
export async function saveConfigAndRescan(next: KytyConfig): Promise<void> {
  await invoke("save_config", { cfg: next });
  await refreshLibrary();
}

export function findGame(gamePath: string): GameEntry | undefined {
  return gamesStore.get().find((g) => g.config.gamePath === gamePath);
}
