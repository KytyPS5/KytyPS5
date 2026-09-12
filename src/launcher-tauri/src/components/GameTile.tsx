import { memo } from "react";
import type { LucideIcon } from "lucide-react";
import type { GameEntry } from "../types";
import { useGameArt } from "./GameCard";
import styles from "./GameTile.module.css";

/** The growing carousel tile in the Home dashboard's top row. Uses the
 * square icon (useGameArt, reused from GameCard.tsx as-is), not the
 * landscape backdrop -- at this size a cropped square icon reads better
 * than a heavily-cropped 4K backdrop.
 *
 * React.memo, paired with Home.tsx passing stable per-item handlers (see
 * its own memoized handler map) -- see GameCard.tsx's identical comment
 * for why both halves are needed together. */
export const GameTile = memo(function GameTile({
  game,
  isActive,
  suppressZoom,
  focusIndex,
  onClick,
  onMouseEnter,
}: {
  game: GameEntry;
  isActive: boolean;
  /** True while a top-row shortcut is hovered or gamepad-focused (Home's
   * own `infoHidden`) -- real bug, 2026-09-09: the previously-selected
   * tile kept its zoom/glow/flux the whole time you were on Library/
   * Settings/Console instead of returning to its idle look like every
   * other unselected tile. `activeIdx` itself is untouched here, only
   * which CLASSES this render picks -- the same tile reappears zoomed the
   * instant this goes false again, same as the info panel it's paired
   * with (Home.tsx's own infoHidden already hides that the same way). */
  suppressZoom?: boolean;
  /** Home's index for this tile in `games`. Read back off the focused
   * element by Home's own focus-follows-selection effect (FocusNav's
   * focusedEl carries no game identity of its own), closing the gap this
   * component used to document below: gamepad/keyboard focus now drives
   * selection too, not only click/hover. */
  focusIndex: number;
  onClick: () => void;
  /** Mouse-hover preview (real PS5 selection behavior). */
  onMouseEnter?: () => void;
}) {
  const art = useGameArt(game);
  const showActive = isActive && !suppressZoom;

  return (
    <div className={styles.tileWrap}>
      <div
        className={showActive ? styles.active : styles.tile}
        data-focusable
        data-game-index={focusIndex}
        data-focus-key={`home-tile-${game.config.gamePath}`}
        onClick={onClick}
        onMouseEnter={onMouseEnter}
      >
        {art ? (
          <img className={styles.image} src={art} alt="" loading="lazy" decoding="async" />
        ) : (
          <div className={styles.placeholder}>{game.config.name.slice(0, 1).toUpperCase()}</div>
        )}
      </div>
      {/* Moving "flux" highlight + shimmer on the selected tile's border,
         while it's also hovered/pressed/focused, not merely selected (see
         .tileFlux/.tileShimmer, GameTile.module.css, for why it stops
         there). Purely decorative, never a click/hover target of its own.
         Keyed off showActive (via .active below), not isActive -- while
         suppressZoom is on, this tile renders as a plain .tile, so the
         .active-only trigger selectors already stop matching, no separate
         suppressZoom check needed here. */}
      <div className={`${styles.tileFlux} flux-ring`} aria-hidden="true" />
      <div className={`${styles.tileShimmer} flux-shimmer`} aria-hidden="true" />
      {/* PS5-style hover/active caption -- large, below the tile, not the old
         9px in-tile label (see GameTile.module.css .caption). */}
      <div className={showActive ? styles.captionActive : styles.caption}>{game.config.name}</div>
    </div>
  );
});

/** The fixed non-game tiles (Library, Settings, Console) at the head of the
 * row. Dark-glass surface matching the "hours played" OSD (.shortcutTile,
 * composes theme.css's --osd-bg/--osd-border) -- not a colored gradient. At
 * rest the border is neutral and the icon is always white; the accent only
 * shows on the border on hover/press (see .shortcutTile:hover/:active in
 * GameTile.module.css) -- never a filled background, and never the icon
 * color. Icons are flat lucide-react glyphs, not generated art -- matches
 * the plan's "painterly arcane for content art, flat lucide icons for
 * chrome" split; these are navigation chrome, not game content. */
export const ShortcutTile = memo(function ShortcutTile({
  id,
  label,
  icon: Icon,
  isActive,
  onClick,
  onMouseEnter,
}: {
  /** Stable, locale-independent identity ("library" / "settings" /
   * "console") -- used for the focus-key data attribute and for Home's
   * activeShortcut selection state. Not derived from `label`: that's a
   * translated string, so keying off it would change the element's
   * identity with the UI language. */
  id: string;
  label: string;
  icon: LucideIcon;
  /** True while this shortcut is the rail's selected item (owner rule,
   * 2026-09-09: same standing-zoom language as a selected GameTile, not
   * only hover/press/focus). Home.tsx only ever sets this in Minimal
   * layout -- Gaming library layout keeps this tile at rest. */
  isActive?: boolean;
  onClick: () => void;
  /** Home's cue to hide/slide out the active-game info block while this
   * shortcut is being hovered (owner rule, 2026-09-09). */
  onMouseEnter?: () => void;
}) {
  return (
    <div className={styles.tileWrap}>
      <div
        className={isActive ? styles.shortcutActive : styles.shortcutTile}
        data-focusable
        data-focus-key={`home-shortcut-${id}`}
        onClick={onClick}
        onMouseEnter={onMouseEnter}
      >
        {/* No forced-visible scrim here (unlike GameTile's own use of this
           class, which does need it) -- ShortcutTile's dark chrome surface
           already gives its label enough contrast on its own; a second
           dark gradient under just one of these three tiles read as a
           stray, inconsistent background rather than a deliberate effect. */}
        <Icon size={20} className={styles.shortcutIcon} />
        <div className={styles.label}>{label}</div>
      </div>
      {/* Moving "flux" highlight + shimmer on the border, on hover/press/
         focus -- siblings, not a ::before on .shortcutTile itself, so they
         can paint above .shortcutTile's own background and win the
         z-index fight against the next rail tile once this one grows (see
         the .tileFlux/.tileShimmer comment, GameTile.module.css). Purely
         decorative, never a click/hover target of their own. */}
      <div className={`${styles.tileFlux} flux-ring`} aria-hidden="true" />
      <div className={`${styles.tileShimmer} flux-shimmer`} aria-hidden="true" />
    </div>
  );
});
