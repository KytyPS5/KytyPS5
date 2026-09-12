import { useEffect, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { ChevronLeft, Copy, LogOut, Maximize2, Minimize2, Minus, Search, Settings, Square, X } from "lucide-react";
import { useStore } from "../store/observable";
import { isRunningStore, runningGameStore } from "../store/run";
import { gamesStore } from "../store/library";
import { useClock } from "../lib/useClock";
import { useWindowControls } from "./useWindowControls";
import type { ViewId } from "../App";
import { useT } from "../i18n";
import { useActiveProfile } from "../lib/profiles";
import styles from "./TopBar.module.css";

export function TopBar({ view, onNavigate, onBack }: { view: ViewId; onNavigate: (v: ViewId) => void; onBack: () => void }) {
  const clock = useClock();
  const t = useT();
  const running = useStore(isRunningStore);
  const runningPath = useStore(runningGameStore);
  const games = useStore(gamesStore);
  const runningGame = games.find((g) => g.config.gamePath === runningPath);
  const activeProfile = useActiveProfile();
  const { maximized, minimize, toggleMaximize, close } = useWindowControls();

  const [fullscreen, setFullscreen] = useState(false);
  useEffect(() => {
    const win = getCurrentWindow();
    void win.isFullscreen().then(setFullscreen);
    const unlisten = win.onResized(() => void win.isFullscreen().then(setFullscreen));
    return () => void unlisten.then((off) => off());
  }, []);
  const toggleFullscreen = () => {
    const win = getCurrentWindow();
    void win.setFullscreen(!fullscreen).then(() => setFullscreen(!fullscreen));
  };

  return (
    <div className={styles.bar} data-focus-chrome>
      {/* No brand mark here anymore -- it's the OS app icon now (see
         src-tauri/icons), not a clickable in-window control. Home shows
         nothing in this slot (there's nowhere further "back" to go); every
         other view shows the chevron. Routes through App.tsx's onBack, not
         a fixed onNavigate("home") -- Game Settings used to need its own
         second back arrow next to the game art (GameDetail.tsx) precisely
         because this one only ever went Home, skipping past the Library
         grid; onBack already knows that one hierarchical step, so this is
         now the only back control and GameDetail's own button is gone. */}
      {view !== "home" && (
        <button
          type="button"
          className={styles.backButton}
          onClick={onBack}
          title={t("common.back")}
          aria-label={t("common.back")}
        >
          <ChevronLeft size={22} strokeWidth={2} />
        </button>
      )}

      {/* Declarative drag region -- Tauri intercepts mousedown here to move
         the window and (with allow-internal-toggle-maximize) double-click to
         toggle maximize. No buttons inside: see TopBar.module.css. */}
      <div className={styles.dragRegion} data-tauri-drag-region />

      <div className={styles.controls}>
        <button type="button" className="icon-button" data-focus-key="topbar-search" onClick={() => onNavigate("library")} title={t("common.search")} aria-label={t("common.search")}>
          <Search size={17} />
        </button>
        <button type="button" className="icon-button" data-focus-key="topbar-settings" onClick={() => onNavigate("settings")} title={t("nav.settings")} aria-label={t("nav.settings")}>
          <Settings size={17} />
        </button>
        <button
          type="button"
          className={styles.profileChip}
          data-focus-key="topbar-profile"
          onClick={() => onNavigate("profile")}
          title={activeProfile?.name ?? t("nav.profile")}
          aria-label={t("nav.profile")}
        >
          {activeProfile ? (
            <span className={styles.profileAvatar}>{activeProfile.name.slice(0, 1).toUpperCase()}</span>
          ) : (
            <span className={styles.profileAvatar} />
          )}
        </button>

        <span className={styles.clock}>{clock}</span>

        {running && (
          <button
            type="button"
            className={styles.runningPill}
            onClick={() => onNavigate("logs")}
            title={runningGame ? t("topbar.runningTooltip", { name: runningGame.config.name }) : t("topbar.aGameIsRunning")}
          >
            <span className="status-dot in-game" style={{ animation: "ps-pulse 1.6s ease-in-out infinite" }} />
            {t("common.running")}
          </button>
        )}

        <button type="button" className="icon-button" onClick={toggleFullscreen} title={fullscreen ? t("topbar.exitFullscreen") : t("topbar.enterFullscreen")}>
          {fullscreen ? <Minimize2 size={16} /> : <Maximize2 size={16} />}
        </button>

        {/* Fullscreen has no window chrome to control -- minimize/maximize
           are meaningless there, and close swaps for a single Exit Launcher
           icon in this same slot (this row would otherwise disappear
           outright). Routes to a confirm page (App.tsx's "exit" view)
           instead of closing immediately, unlike the window-chrome close
           button below. */}
        {!fullscreen && (
          <div className={styles.windowButtons}>
            <button type="button" className={styles.winBtn} onClick={minimize} title={t("topbar.minimize")}>
              <Minus size={16} />
            </button>
            <button type="button" className={styles.winBtn} onClick={toggleMaximize} title={maximized ? t("topbar.restore") : t("topbar.maximize")}>
              {maximized ? <Copy size={13} /> : <Square size={13} />}
            </button>
            <button type="button" className={`${styles.winBtn} ${styles.winBtnClose}`} onClick={close} title={t("common.close")}>
              <X size={16} />
            </button>
          </div>
        )}
        {fullscreen && (
          <div className={styles.windowButtons}>
            <button
              type="button"
              className={`${styles.winBtn} ${styles.winBtnClose}`}
              onClick={() => onNavigate("exit")}
              title={t("topbar.exitLauncher")}
              aria-label={t("topbar.exitLauncher")}
            >
              <LogOut size={16} />
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
