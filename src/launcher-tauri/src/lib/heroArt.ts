import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { convertFileSrc } from "@tauri-apps/api/core";
import type { GameEntry } from "../types";
import { artKeyFor } from "../components/GameCard";

const COVER_PLACEHOLDER_COUNT = 6;

/** Deterministic per-title placeholder assignment -- same game always gets
 * the same generated cover, instead of a random one on every relaunch. */
function hashToPlaceholder(key: string): string {
  let h = 0;
  for (let i = 0; i < key.length; i++) {
    h = (h * 31 + key.charCodeAt(i)) | 0;
  }
  const index = (Math.abs(h) % COVER_PLACEHOLDER_COUNT) + 1;
  return `/art/cover_placeholder_${String(index).padStart(2, "0")}.png`;
}

export type HeroArtKind = "backdrop" | "icon" | "placeholder";

export interface HeroArt {
  url: string;
  kind: HeroArtKind;
  /** Real backdrop art (sce_sys/pic0.png, typically landscape 4K) needs no
   * blur -- it already looks intentional as a full-bleed background. The
   * square icon and generated placeholders are cropped-to-cover and need
   * blur to hide that they were never meant to be a landscape hero (see
   * blurCache.ts, and the Step 0b measurement on why that blur must be
   * pre-baked rather than a live CSS filter). */
  needsBlur: boolean;
}

/** Resolves the best available hero-background source for a game:
 * a user's custom art override > sce_sys/pic0.png (a real landscape
 * backdrop most legitimately-dumped titles ship, GameEntry.backdropPath --
 * discovered mid-build; the pre-existing useGameArt() in GameCard.tsx never
 * surfaced it, only iconPath) > sce_sys/icon0.png (square, needs blur) >
 * a deterministic generated placeholder (square, needs blur). */
export function useHeroArt(game: GameEntry | null): HeroArt | null {
  const [art, setArt] = useState<HeroArt | null>(null);

  useEffect(() => {
    if (!game) {
      setArt(null);
      return;
    }
    // Clear synchronously, not only when `game` itself goes null -- same
    // fix as useGameArt's own `setPath(null)` (GameCard.tsx), applied here
    // too (real bug, 2026-09-09: reported as "hovering a game with no art
    // shows the LAST game's art"). Without this, switching from a game
    // with real art to one with none left the previous game's resolved
    // `art` on screen for the whole async invoke below, since nothing
    // reset it in between -- Home.tsx's own effect then kept publishing
    // that stale backdrop as the hero override instead of falling back to
    // the default dashboard background. Clearing to null here makes
    // Home's effect take its `else` branch (clearHeroOverride) for the gap,
    // which is exactly the correct default-background look while this
    // resolves, not a new flash of its own.
    setArt(null);
    let cancelled = false;
    void (async () => {
      const artKey = artKeyFor(game.config);
      // Real bug (2026-09-09): this invoke was uncaught, so a Rust-side
      // error (e.g. a game whose sce_sys layout the override lookup doesn't
      // expect) threw out of this async IIFE before setArt ever ran, leaving
      // `art` stuck at its initial null forever -- Home's hero background
      // then rendered nothing (solid black) instead of falling through to
      // backdropPath/iconPath/placeholder like every other failure path
      // here already does.
      let override: string | null = null;
      try {
        override = await invoke<string | null>("get_game_art", { artKey });
      } catch {
        // Fall through to the same backdrop/icon/placeholder chain below.
      }
      if (cancelled) return;

      if (override) {
        setArt({ url: convertFileSrc(override), kind: "backdrop", needsBlur: false });
      } else if (game.backdropPath) {
        setArt({ url: convertFileSrc(game.backdropPath), kind: "backdrop", needsBlur: false });
      } else if (game.iconPath) {
        setArt({ url: convertFileSrc(game.iconPath), kind: "icon", needsBlur: true });
      } else {
        setArt({ url: hashToPlaceholder(artKey), kind: "placeholder", needsBlur: true });
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [game?.config.gamePath, game?.iconPath, game?.backdropPath]);

  return art;
}
