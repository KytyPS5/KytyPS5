import { invoke } from "@tauri-apps/api/core";
import type { GameEntry, PlayHistory } from "../types";
import { createStore } from "./observable";
import { t } from "../i18n";

export interface LibraryStats {
  totalGamesPlayed: number;
  totalPlayCount: number;
  totalSeconds: number;
}

export const playHistoryStore = createStore<PlayHistory>({});
export const libraryStatsStore = createStore<LibraryStats>({ totalGamesPlayed: 0, totalPlayCount: 0, totalSeconds: 0 });

export async function refreshPlayHistory(): Promise<void> {
  const [history, stats] = await Promise.all([
    invoke<PlayHistory>("get_play_history"),
    invoke<LibraryStats>("get_library_stats"),
  ]);
  playHistoryStore.set(history);
  libraryStatsStore.set(stats);
}

export function recentlyPlayed(games: GameEntry[], history: PlayHistory, limit = 12): GameEntry[] {
  return games
    .filter((g) => history[g.config.gamePath])
    .sort((a, b) => history[b.config.gamePath].lastPlayedMs - history[a.config.gamePath].lastPlayedMs)
    .slice(0, limit);
}

/** By total time played, not recency -- the Profile overview's "Most
 * played" card (distinct from "Recently played," which the same page and
 * Home both already show). */
export function mostPlayed(games: GameEntry[], history: PlayHistory, limit = 3): GameEntry[] {
  return games
    .filter((g) => history[g.config.gamePath]?.totalSeconds)
    .sort((a, b) => history[b.config.gamePath].totalSeconds - history[a.config.gamePath].totalSeconds)
    .slice(0, limit);
}

export function formatPlaytime(totalSeconds: number): string {
  if (totalSeconds < 60) return t("playtime.lessThanMinute");
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.round((totalSeconds % 3600) / 60);
  if (hours === 0) return t("playtime.minutes", { count: minutes });
  if (minutes === 0) return t("playtime.hours", { count: hours });
  return t("playtime.hoursMinutes", { hours, minutes });
}
