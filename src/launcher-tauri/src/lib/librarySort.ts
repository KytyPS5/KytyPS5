import { createStore, useStore } from "../store/observable";
import type { CompatibilityMap, GameEntry, GameStatus, PlayHistory } from "../types";

export type SortId = "nameAsc" | "nameDesc" | "recent" | "played" | "addedNew" | "addedOld";

export interface LibraryFilters {
  /** Empty means "any status" -- never an explicit list of all five. */
  statuses: GameStatus[];
  played: "all" | "played" | "never";
  customOnly: boolean;
}

export const DEFAULT_FILTERS: LibraryFilters = { statuses: [], played: "all", customOnly: false };

const SORT_KEY = "kyty.librarySort";
const FILTERS_KEY = "kyty.libraryFilters";

const SORT_IDS: SortId[] = ["nameAsc", "nameDesc", "recent", "played", "addedNew", "addedOld"];

function readStoredSort(): SortId {
  try {
    const v = localStorage.getItem(SORT_KEY);
    return (SORT_IDS as string[]).includes(v ?? "") ? (v as SortId) : "nameAsc";
  } catch {
    return "nameAsc";
  }
}

function readStoredFilters(): LibraryFilters {
  try {
    const raw = localStorage.getItem(FILTERS_KEY);
    if (!raw) return DEFAULT_FILTERS;
    const parsed = JSON.parse(raw);
    return {
      statuses: Array.isArray(parsed.statuses) ? parsed.statuses : [],
      played: parsed.played === "played" || parsed.played === "never" ? parsed.played : "all",
      customOnly: parsed.customOnly === true,
    };
  } catch {
    return DEFAULT_FILTERS;
  }
}

const librarySortStore = createStore<SortId>(readStoredSort());
const libraryFiltersStore = createStore<LibraryFilters>(readStoredFilters());

export function useLibrarySort(): SortId {
  return useStore(librarySortStore);
}

export function setLibrarySort(sort: SortId): void {
  librarySortStore.set(sort);
  try {
    localStorage.setItem(SORT_KEY, sort);
  } catch {
    // Best-effort persistence only -- see readStoredSort's catch.
  }
}

export function useLibraryFilters(): LibraryFilters {
  return useStore(libraryFiltersStore);
}

export function setLibraryFilters(filters: LibraryFilters): void {
  libraryFiltersStore.set(filters);
  try {
    localStorage.setItem(FILTERS_KEY, JSON.stringify(filters));
  } catch {
    // Best-effort persistence only.
  }
}

export function hasActiveFilters(filters: LibraryFilters): boolean {
  return filters.statuses.length > 0 || filters.played !== "all" || filters.customOnly;
}

/** Search + filter + sort in one pass, in that order -- search and filters
 * narrow the set, sort only ever reorders what survived. Pure function so
 * the panel and the grid can both call it without duplicating the logic. */
export function applyLibraryView(
  games: GameEntry[],
  history: PlayHistory,
  compatibility: CompatibilityMap,
  query: string,
  filters: LibraryFilters,
  sort: SortId,
): GameEntry[] {
  const q = query.trim().toLowerCase();

  let result = games.filter((g) => {
    if (q && !g.config.name.toLowerCase().includes(q) && !g.config.titleId.toLowerCase().includes(q)) {
      return false;
    }
    if (filters.statuses.length > 0) {
      const status = compatibility[g.config.titleId.toUpperCase()]?.status ?? "Unknown";
      if (!filters.statuses.includes(status)) return false;
    }
    if (filters.played !== "all") {
      const hasPlayed = Boolean(history[g.config.gamePath]);
      if (filters.played === "played" && !hasPlayed) return false;
      if (filters.played === "never" && hasPlayed) return false;
    }
    if (filters.customOnly && !g.config.customSettings) return false;
    return true;
  });

  result = [...result].sort((a, b) => {
    switch (sort) {
      case "nameAsc":
        return a.config.name.localeCompare(b.config.name);
      case "nameDesc":
        return b.config.name.localeCompare(a.config.name);
      case "recent": {
        const av = history[a.config.gamePath]?.lastPlayedMs ?? 0;
        const bv = history[b.config.gamePath]?.lastPlayedMs ?? 0;
        return bv - av || a.config.name.localeCompare(b.config.name);
      }
      case "played": {
        const av = history[a.config.gamePath]?.totalSeconds ?? 0;
        const bv = history[b.config.gamePath]?.totalSeconds ?? 0;
        return bv - av || a.config.name.localeCompare(b.config.name);
      }
      case "addedNew":
        return b.firstSeenMs - a.firstSeenMs || a.config.name.localeCompare(b.config.name);
      case "addedOld":
        return a.firstSeenMs - b.firstSeenMs || a.config.name.localeCompare(b.config.name);
      default:
        return 0;
    }
  });

  return result;
}
