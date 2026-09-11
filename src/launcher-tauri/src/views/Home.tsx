import { useEffect, useMemo, useRef, useState } from "react";
import { LibraryBig, MoreHorizontal, Play, Settings, Square, Terminal, Trophy } from "lucide-react";
import type { ViewId } from "../App";
import type { GameEntry } from "../types";
import { useStore } from "../store/observable";
import { gamesStore } from "../store/library";
import { isRunningStore, runningGameStore, runGame, stopGame } from "../store/run";
import { GameCard } from "../components/GameCard";
import { GameTile, ShortcutTile } from "../components/GameTile";
import { setHeroOverride, clearHeroOverride } from "../store/background";
import { useHomeLayout } from "../lib/homeLayout";
import { useHeroArt } from "../lib/heroArt";
import { useAccentColor, useHeroDisplayUrl } from "../lib/accent";
import { useRailOffset } from "../lib/useRailOffset";
import { useFocusNav } from "../nav/FocusNav";
import { useTrophyCounts, useEarnedFor, trophyTotal } from "../store/trophies";
import { useT } from "../i18n";
import styles from "./Home.module.css";

/** ShortcutTile stamps data-focus-key="home-shortcut-<id>" (GameTile.tsx),
 * GameTile stamps "home-tile-<gamePath>", GameCard stamps "card-<gamePath>"
 * (GameCard.tsx) -- reading focusedEl's own key back is the gamepad/
 * keyboard half of Home's activeShortcut. undefined means "the focus
 * channel has no opinion right now" (nothing gamepad/keyboard-focused, or
 * focus is on something outside this set entirely), which the caller falls
 * through to the mouse channel for -- see pointerShortcut's own comment in
 * HomeView for why this is a plain derived read, not a useEffect writing
 * its own useState. */
function shortcutFromFocusKey(key: string | undefined): string | null | undefined {
  if (key === undefined) return undefined;
  if (key.startsWith("home-shortcut-")) return key.slice("home-shortcut-".length);
  if (key.startsWith("home-tile-") || key.startsWith("card-")) return null;
  return undefined;
}

export function HomeView({ onNavigate, onViewDetails }: { onNavigate: (v: ViewId) => void; onViewDetails: (gamePath: string) => void }) {
  const games = useStore(gamesStore);
  const running = useStore(isRunningStore);
  const runningPath = useStore(runningGameStore);
  const layout = useHomeLayout();
  const t = useT();

  const [activeIdx, setActiveIdx] = useState(0);
  const [playError, setPlayError] = useState<string | null>(null);
  // Which top-row shortcut (Library/Settings/Console) is the rail's
  // selected item -- id or null. Doubles as the cue to hide/slide out the
  // active-game info block (title, badges, Play, the "..." button) while a
  // shortcut is selected (owner rule, 2026-09-09). This is only the MOUSE
  // half now (set directly by each tile's onMouseEnter, and cleared on rail
  // mouse-leave below); gamepad/keyboard is derived from focusedEl every
  // render instead of written in here too (see activeShortcut below) --
  // real bug, 2026-09-09: with a single useState written by both channels,
  // FocusNav clearing focusedEl to null (its mousemove-deactivate effect,
  // or a view-change reset) left whatever the focus channel had last
  // written standing forever, since nothing then told this state to go
  // back to null. That stranded the info panel hidden (and the hovered
  // tile de-zoomed, losing .active and its flux) even while the mouse was
  // parked on a game tile with no shortcut focused at all -- the two
  // screenshots' reported bug. In Minimal layout this still persists after
  // the pointer/focus leaves the shortcut (owner rule, 2026-09-09: same
  // standing-zoom language as a selected GameTile); Gaming library layout
  // clears it on mouse-leave instead, since that layout never zooms
  // shortcuts at all.
  const [pointerShortcut, setPointerShortcut] = useState<string | null>(null);
  const { focusedEl } = useFocusNav();

  const railViewportRef = useRef<HTMLDivElement>(null);
  const railTrackRef = useRef<HTMLDivElement>(null);
  useRailOffset(railViewportRef, railTrackRef, layout === "standard" ? activeIdx : null);

  useEffect(() => {
    if (activeIdx >= games.length) setActiveIdx(0);
  }, [games.length, activeIdx]);

  useEffect(() => {
    setPlayError(null);
  }, [activeIdx]);

  // Gamepad/keyboard focus now drives the hero, not only click/hover --
  // closes the gap GameTile.tsx used to document. FocusNav's focusedEl
  // carries no game identity of its own, so rail tiles and library cards
  // stamp their own index as data-game-index and this reads it back.
  useEffect(() => {
    const raw = focusedEl?.dataset.gameIndex;
    if (raw === undefined) return;
    const idx = Number(raw);
    if (Number.isNaN(idx) || idx === activeIdx) return;
    setActiveIdx(idx);
  }, [focusedEl, activeIdx]);

  const activeGame: GameEntry | null = games[activeIdx] ?? null;

  // Stable per-item handlers, paired with GameTile/GameCard/ShortcutTile's
  // own React.memo -- see those components' doc comments for why both
  // halves matter together. Rebuilt only when `games` itself changes (a
  // library rescan), not on every hover: before this, `games.map((g, i) =>
  // <GameTile onClick={() => setActiveIdx(i)} ...>)` allocated a fresh
  // closure for every tile on every render, which defeats memo outright --
  // a "new" onClick reference fails memo's shallow prop comparison even
  // when nothing about that tile actually changed, so every tile in the
  // grid re-rendered on every single hover regardless of which one moved.
  const perGameHandlers = useMemo(
    () =>
      games.map((g, i) => ({
        onClick: () => setActiveIdx(i),
        onMouseEnter: () => {
          setActiveIdx(i);
          setPointerShortcut(null);
        },
        onDoubleClick: () => void runGame(g.config, g.config.titleId),
        onContextMenu: (e: React.MouseEvent) => e.preventDefault(),
      })),
    [games],
  );

  // Derived on every render, not written into a useEffect + its own
  // useState the way this used to work (see pointerShortcut's own comment
  // above for the bug that caused: an effect writing null/absent focus into
  // the same state a mouse handler also wrote left the last-written value
  // stranded once focus cleared).
  const focusShortcut = shortcutFromFocusKey(focusedEl?.dataset.focusKey);
  const activeShortcut = focusShortcut !== undefined ? focusShortcut : pointerShortcut;

  const infoHidden = activeShortcut !== null;

  // Per-game hero art (confirmed decision: reverses the earlier
  // "fixed theme only" call). Published into store/background.ts rather
  // than rendered here directly -- AppShell owns the one HeroBackground
  // instance for the whole app now (real bug, 2026-09-09: a per-view
  // instance reset its crossfade state, and briefly the base background
  // itself, to black on every navigation).
  const heroArt = useHeroArt(activeGame);
  const heroUrl = useHeroDisplayUrl(heroArt);
  const heroAccent = useAccentColor(heroArt);

  useEffect(() => {
    // Gated on whether resolution has actually SETTLED, not on a guessed
    // time window. An earlier version of this effect debounced the clear
    // path with a fixed 120ms timer to smooth over heroArt.ts's synchronous
    // per-game reset -- real bug, found live (2026-09-10): a game with no
    // real art (Worms Armageddon: icon/backdrop absent, falls through to a
    // generated placeholder) resolves through several async hops (art ->
    // needsBlur's blur pipeline -> accent color), each landing moments
    // apart and each re-arming that timer via this effect's own
    // dependency list -- so the clear could be rescheduled indefinitely
    // and never actually fire, leaving the PREVIOUS game's hero visibly
    // stuck on screen instead of falling back to the default background.
    //
    // The actual fix needs no timer at all: only touch the store once we
    // KNOW the answer.
    //   - heroArt === null: this game's art hasn't resolved yet at all --
    //     leave the override exactly as it is (whatever was last settled
    //     keeps showing) rather than clearing now and setting again the
    //     moment it resolves, which is what caused the old fade-out-then-
    //     in on every hover.
    //   - heroArt.kind === "placeholder": definitively no real art for
    //     this game -- clear immediately. No need to also wait for
    //     heroUrl: Home never shows a placeholder as hero art regardless
    //     of whether its blur has finished.
    //   - heroArt has real art (backdrop/icon) but heroUrl hasn't resolved
    //     yet (still blurring, or not yet loaded) -- leave the override
    //     alone, same reasoning as the first case.
    //   - heroArt has real art AND heroUrl is ready -- set it.
    if (!activeGame) {
      clearHeroOverride();
      return;
    }
    if (!heroArt) return;
    if (heroArt.kind === "placeholder") {
      clearHeroOverride();
      return;
    }
    if (!heroUrl) return;
    setHeroOverride({ url: heroUrl, accent: heroAccent });
  }, [activeGame, heroArt, heroUrl, heroAccent]);

  // The actual unmount cleanup (Home navigated away from entirely) that the
  // effect above used to also (incorrectly) run on every dependency change.
  useEffect(() => {
    return () => clearHeroOverride();
  }, []);

  const trophyCounts = useTrophyCounts(activeGame?.config.basedir);
  const trophyEarned = useEarnedFor(activeGame?.config.basedir);
  const trophyTotalCount = trophyCounts ? trophyTotal(trophyCounts) : 0;

  const isActiveGameRunning = running && runningPath === activeGame?.config.gamePath;
  const playDisabled = running && !isActiveGameRunning;

  const handlePlay = async () => {
    if (!activeGame) return;
    setPlayError(null);
    try {
      if (isActiveGameRunning) {
        await stopGame();
      } else if (!playDisabled) {
        await runGame(activeGame.config, activeGame.config.titleId);
      }
    } catch (e) {
      setPlayError(e instanceof Error ? e.message : String(e));
    }
  };

  return (
    <div className={styles.root} data-home-layout={layout}>
      {/* AppShell mounts the one shell-level HeroBackground now (see
         store/background.ts's publish effect above) -- Home just wraps its
         own content in a stacked-above div like every other view, since
         HeroBackground.module.css's .root carries its own z-index. */}
      <div className={styles.content}>
      <div className={styles.sectionLabel}>{t("home.gamesLabel")}</div>
      {/* Viewport + transformed track (03-components-and-layout.md), not the
         old overflow-x:auto scroll container -- see useRailOffset for the
         measurement and GameTile.module.css for why growth on selection no
         longer touches layout at all. */}
      <div
        className={styles.railViewport}
        ref={railViewportRef}
        onMouseLeave={() => {
          // Minimal layout: a selected shortcut stays zoomed after the
          // pointer leaves the row (owner rule, 2026-09-09). Gaming
          // library layout never zooms shortcuts, so it keeps the old
          // "info panel returns on mouse-leave" behavior unchanged.
          if (layout !== "standard") setPointerShortcut(null);
        }}
      >
        <div className={styles.railTrack} ref={railTrackRef}>
          <ShortcutTile
            id="library"
            label={t("nav.library")}
            icon={LibraryBig}
            isActive={layout === "standard" && activeShortcut === "library"}
            onClick={() => onNavigate("library")}
            onMouseEnter={() => setPointerShortcut("library")}
          />
          <ShortcutTile
            id="settings"
            label={t("nav.settings")}
            icon={Settings}
            isActive={layout === "standard" && activeShortcut === "settings"}
            onClick={() => onNavigate("settings")}
            onMouseEnter={() => setPointerShortcut("settings")}
          />
          <ShortcutTile
            id="console"
            label={t("nav.console")}
            icon={Terminal}
            isActive={layout === "standard" && activeShortcut === "console"}
            onClick={() => onNavigate("logs")}
            onMouseEnter={() => setPointerShortcut("console")}
          />
          {layout === "standard" &&
            games.map((g, i) => (
              <GameTile
                key={g.config.gamePath}
                game={g}
                isActive={i === activeIdx}
                suppressZoom={infoHidden}
                focusIndex={i}
                onClick={perGameHandlers[i].onClick}
                onMouseEnter={perGameHandlers[i].onMouseEnter}
              />
            ))}
        </div>
      </div>

      {layout === "library" && games.length > 0 ? (
        <div className={styles.libraryGrid} data-scroll-region>
          {games.map((g, i) => (
            <GameCard
              key={g.config.gamePath}
              game={g}
              selected={i === activeIdx}
              focusIndex={i}
              onClick={perGameHandlers[i].onClick}
              onDoubleClick={perGameHandlers[i].onDoubleClick}
              onMouseEnter={perGameHandlers[i].onMouseEnter}
              onContextMenu={perGameHandlers[i].onContextMenu}
            />
          ))}
        </div>
      ) : (
        <div className={styles.spacer} />
      )}

      {activeGame ? (
        <div className={styles.bottomRow}>
          <div className={`${styles.info} ${infoHidden ? styles.infoHidden : ""}`}>
            <h1 className={styles.title}>{activeGame.config.name}</h1>
            <div className={styles.chips}>
              {activeGame.config.titleId && <span className={styles.chip}>{activeGame.config.titleId}</span>}
              {activeGame.config.gameVersion && <span className={styles.chip}>{t("home.versionPrefix", { value: activeGame.config.gameVersion })}</span>}
              {activeGame.config.firmwareVer && <span className={styles.chip}>{t("home.firmwarePrefix", { value: activeGame.config.firmwareVer })}</span>}
            </div>
            <div className={styles.ctaRow}>
              <button type="button" data-focusable className={`${styles.playButton} ${isActiveGameRunning ? styles.stop : ""}`} disabled={playDisabled} onClick={handlePlay}>
                {isActiveGameRunning ? <Square size={14} fill="currentColor" /> : <Play size={16} fill="#000" />}
                {isActiveGameRunning ? t("home.stop") : t("home.play")}
              </button>
              <button type="button" data-focusable className={styles.moreButton} title={t("home.viewDetails")} onClick={() => onViewDetails(activeGame.config.gamePath)}>
                <MoreHorizontal size={18} />
              </button>
            </div>
            {playError && <div className={styles.playError}>{playError}</div>}
          </div>

          <div className={styles.panels}>
            {/* Real trophy-pack data (store/trophies.ts) in the same spot the
               reference PS5 dashboard uses for its own trophy OSD. Earned is
               always 0 today -- nothing in the emulator unlocks a trophy yet
               -- so this only ever shows Progress 0% / Earned 0/<total>,
               matching the reference's own honest "0%, 0/46" shape rather
               than fabricating any progress. Hidden entirely for a game with
               no trophy pack on disk. */}
            {trophyTotalCount > 0 && (
              <div className={styles.trophyOsd}>
                <Trophy size={20} className={styles.trophyIcon} />
                <div className={styles.trophyCols}>
                  <div className={styles.trophyCol}>
                    <div className={styles.trophyLabel}>{t("trophies.progress")}</div>
                    <div className={styles.trophyValue}>{Math.round((trophyEarned / trophyTotalCount) * 100)}%</div>
                  </div>
                  <div className={styles.trophyCol}>
                    <div className={styles.trophyLabel}>{t("trophies.earned")}</div>
                    <div className={styles.trophyValue}>{trophyEarned}/{trophyTotalCount}</div>
                  </div>
                </div>
              </div>
            )}
          </div>
        </div>
      ) : (
        <div className={styles.emptyBottomRow}>
          <div className={styles.emptyHint}>{t("home.emptyHint")}</div>
        </div>
      )}
      </div>
    </div>
  );
}
