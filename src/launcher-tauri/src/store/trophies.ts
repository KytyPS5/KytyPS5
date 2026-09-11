// Trophy counts for the dashboard/Profile OSD (Home.tsx, Profile.tsx).
// Follows store/playtime.ts's shape: a plain Store, plus hooks that fetch
// once per basedir and cache the result, so moving the Home rail selection
// or opening Profile never re-parses a .ucp pack that's already known.

import { useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";
import { createStore, useStore } from "./observable";
import type { GameEntry } from "../types";

export interface TrophyCounts {
  platinum: number;
  gold: number;
  silver: number;
  bronze: number;
}

export function trophyTotal(c: TrophyCounts): number {
  return c.platinum + c.gold + c.silver + c.bronze;
}

const EMPTY: TrophyCounts = { platinum: 0, gold: 0, silver: 0, bronze: 0 };

/** basedir -> counts, filled in lazily as games are queried. */
const countsStore = createStore<Record<string, TrophyCounts>>({});

/** basedir -> trophies actually earned so far. Nothing in this stack
 * unlocks a trophy yet (no sceNpTrophy implementation -- see trophy.rs's
 * TrophyCounts doc), so this stays empty and every read is 0. It exists as
 * the one seam a future emulator-side unlock hook needs to write to; no
 * component computes "earned" any other way. */
export const trophyEarnedStore = createStore<Record<string, number>>({});

const inFlight = new Set<string>();

async function ensureLoaded(basedir: string): Promise<void> {
  if (!basedir || basedir in countsStore.get() || inFlight.has(basedir)) return;
  inFlight.add(basedir);
  try {
    const counts = await invoke<TrophyCounts>("get_trophy_counts", { basedir });
    countsStore.update((prev) => ({ ...prev, [basedir]: counts }));
  } catch {
    countsStore.update((prev) => ({ ...prev, [basedir]: EMPTY }));
  } finally {
    inFlight.delete(basedir);
  }
}

/** Counts for one game, fetched on first use and cached thereafter. */
export function useTrophyCounts(basedir: string | null | undefined): TrophyCounts | null {
  const all = useStore(countsStore);
  useEffect(() => {
    if (basedir) void ensureLoaded(basedir);
  }, [basedir]);
  if (!basedir) return null;
  return all[basedir] ?? null;
}

/** Library-wide totals (Profile page): fetches every basedir not already
 * cached in one batch call, then sums. */
export function useLibraryTrophyCounts(games: GameEntry[]): TrophyCounts {
  const all = useStore(countsStore);
  const basedirs = games.map((g) => g.config.basedir).filter(Boolean);
  const key = basedirs.join("|");

  useEffect(() => {
    const missing = basedirs.filter((b) => !(b in countsStore.get()) && !inFlight.has(b));
    if (missing.length === 0) return;
    for (const b of missing) inFlight.add(b);
    void invoke<Record<string, TrophyCounts>>("get_trophy_counts_batch", { basedirs: missing })
      .then((result) => {
        countsStore.update((prev) => ({ ...prev, ...result }));
      })
      .catch(() => {
        countsStore.update((prev) => {
          const next = { ...prev };
          for (const b of missing) next[b] = EMPTY;
          return next;
        });
      })
      .finally(() => {
        for (const b of missing) inFlight.delete(b);
      });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);

  return basedirs.reduce<TrophyCounts>(
    (sum, b) => {
      const c = all[b] ?? EMPTY;
      return { platinum: sum.platinum + c.platinum, gold: sum.gold + c.gold, silver: sum.silver + c.silver, bronze: sum.bronze + c.bronze };
    },
    { ...EMPTY },
  );
}

/** Total trophies earned across the whole library, from trophyEarnedStore. */
export function useLibraryEarned(games: GameEntry[]): number {
  const earned = useStore(trophyEarnedStore);
  return games.reduce((sum, g) => sum + (earned[g.config.basedir] ?? 0), 0);
}

/** Earned count for one game, reactive (subscribes to trophyEarnedStore). */
export function useEarnedFor(basedir: string | null | undefined): number {
  const earned = useStore(trophyEarnedStore);
  return basedir ? (earned[basedir] ?? 0) : 0;
}
