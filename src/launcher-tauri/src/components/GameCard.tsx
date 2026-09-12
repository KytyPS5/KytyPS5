import { memo, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { convertFileSrc } from "@tauri-apps/api/core";
import type { GameEntry } from "../types";
import styles from "./GameCard.module.css";

export const STATUS_CLASS: Record<string, string> = {
  InGame: "in-game",
  MainMenu: "main-menu",
  Logo: "logo",
  DoesntBoot: "doesnt-boot",
  Unknown: "unknown",
};

export function artKeyFor(game: GameEntry["config"]): string {
  return game.titleId || game.gamePath;
}

/** Cover-art priority: user override > sce_sys/icon0.png from the dump >
 * a generic placeholder tile. No networked fetch — see art.rs. */
export function useGameArt(game: GameEntry) {
  const [path, setPath] = useState<string | null>(null);

  useEffect(() => {
    // Real bug: switching games left the *previous* game's art on screen
    // (GameDetail's backdrop) until this async get_game_art call resolved,
    // since nothing reset `path` in between -- clearing it synchronously
    // here means a game with no art briefly shows no art, not the wrong
    // game's art.
    setPath(null);
    let cancelled = false;
    void (async () => {
      const override = await invoke<string | null>("get_game_art", { artKey: artKeyFor(game.config) });
      if (!cancelled) setPath(override ?? game.iconPath ?? null);
    })();
    return () => {
      cancelled = true;
    };
  }, [game.config.gamePath, game.iconPath]);

  return path ? convertFileSrc(path) : null;
}

// React.memo, paired with callers passing stable per-item handlers (see
// Home.tsx's/Library.tsx's own memoized handler maps) -- without a stable
// handler identity too, memo does nothing: a new `onClick` closure every
// render still fails the shallow prop comparison and re-renders anyway.
// Before this, hovering any one tile in a grid of N games re-rendered all
// N GameCards, not just the one (at most two: previously- and newly-
// selected) whose own props actually changed.
export const GameCard = memo(function GameCard({
  game,
  selected,
  status,
  focusIndex,
  captionMode = "always",
  onClick,
  onDoubleClick,
  onContextMenu,
  onMouseEnter,
}: {
  game: GameEntry;
  selected: boolean;
  status?: string;
  /** Optional: the caller's index for this card (Home's library grid uses
   * it to let gamepad/keyboard focus drive `activeIdx`, not just click/hover
   * -- see data-game-index below). Callers with no such concept (Library,
   * Profile's recently-played row) simply omit it. */
  focusIndex?: number;
  /** "always" (default): the caption always shows, every existing call site.
   * "selected": the caption element still renders (so the grid never
   * reflows) but is invisible until this card is `selected` -- the PS5
   * Game Library reference names only the focused tile. */
  captionMode?: "always" | "selected";
  onClick: () => void;
  onDoubleClick: () => void;
  onContextMenu: (e: React.MouseEvent) => void;
  onMouseEnter?: () => void;
}) {
  const art = useGameArt(game);

  return (
    <div className={styles.card} onClick={onClick} onDoubleClick={onDoubleClick} onContextMenu={onContextMenu} onMouseEnter={onMouseEnter}>
      {/* data-focusable lives on the art square alone, not this whole card
         -- the PS5 reference (and this app's own GameTile carousel) only
         ever boxes the artwork on focus/select; a ring wrapping the image
         AND the caption text below it reads as a bug, not a selection. */}
      <div
        data-focusable
        data-game-index={focusIndex}
        data-focus-key={`card-${game.config.gamePath}`}
        className={`${styles.art} ${art ? styles.artHasImage : styles.artNoImage} ${selected ? styles.artSelected : ""}`}
      >
        {art ? (
          <img className={styles.image} src={art} alt="" loading="lazy" decoding="async" />
        ) : (
          <div className={styles.placeholder}>{game.config.name.slice(0, 1).toUpperCase()}</div>
        )}
        {status && <span className={`status-dot ${STATUS_CLASS[status] ?? "unknown"} ${styles.statusDot}`} />}
        {/* Moving "flux" highlight + shimmer while this card is hovered,
           gamepad-focused, or pressed (see .artFlux/.artShimmer,
           GameCard.module.css). Purely decorative, never a click target of
           its own. */}
        <div className={`${styles.artFlux} flux-ring`} aria-hidden="true" />
        <div className={`${styles.artShimmer} flux-shimmer`} aria-hidden="true" />
      </div>
      <div className={`${styles.caption} ${captionMode === "selected" && !selected ? styles.captionHidden : ""}`}>
        {game.config.name}
      </div>
    </div>
  );
});
