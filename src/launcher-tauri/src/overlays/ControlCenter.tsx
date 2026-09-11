import { useEffect, useRef } from "react";
import { Gamepad2, Power, Settings as SettingsIcon, Square, Terminal, User, Volume2, VolumeX } from "lucide-react";
import type { ViewId } from "../App";
import { useStore } from "../store/observable";
import { gamesStore } from "../store/library";
import { isRunningStore, runningGameStore, stopGame } from "../store/run";
import { useAudioSettings, setSfxEnabled } from "../lib/audioSettings";
import { requestSettingsCategory } from "../lib/settingsNav";
import { useFocusNav } from "../nav/FocusNav";
import { useT } from "../i18n";
import overlayStyles from "./Overlay.module.css";
import { useOverlayLifecycle } from "./useOverlayLifecycle";
import styles from "./ControlCenter.module.css";

/** The reference's control-center overlay: appears over the live home scene
 * (dims rather than replacing it -- Overlay.module.css's .backdrop) with
 * quick access to the things a player reaches for mid-session. Everything
 * here is a deep link into existing screens/state (lib/settingsNav.ts,
 * store/run.ts, lib/audioSettings.ts) -- no new backend commands. */
export function ControlCenter({
  open,
  onClose,
  onNavigate,
  onOpenPower,
}: {
  open: boolean;
  onClose: () => void;
  onNavigate: (v: ViewId) => void;
  onOpenPower: () => void;
}) {
  const { mounted, isOpen } = useOverlayLifecycle(open, 280);
  const rootRef = useRef<HTMLDivElement>(null);
  const { pushFocusScope, popFocusScope } = useFocusNav();
  const t = useT();

  const running = useStore(isRunningStore);
  const runningPath = useStore(runningGameStore);
  const games = useStore(gamesStore);
  const runningGame = games.find((g) => g.config.gamePath === runningPath);
  const audio = useAudioSettings();

  useEffect(() => {
    if (!open || !rootRef.current) return;
    pushFocusScope(rootRef.current, onClose);
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);

  if (!mounted) return null;

  const goto = (view: ViewId, category?: string) => {
    if (category) requestSettingsCategory(category);
    onNavigate(view);
    onClose();
  };

  return (
    <div className={`${overlayStyles.backdrop} ${isOpen ? overlayStyles.isOpen : ""}`} onClick={onClose}>
      <div ref={rootRef} className={`${overlayStyles.panel} ${styles.panel}`} onClick={(e) => e.stopPropagation()}>
        <div className={styles.title}>{t("controlCenter.title")}</div>

        {running && runningGame && (
          <div className={styles.row}>
            <span className={styles.rowLabel}>{runningGame.config.name}</span>
            <button type="button" data-focusable className="pill-button" onClick={() => void stopGame()}>
              <Square size={13} fill="currentColor" />
              {t("controlCenter.stop")}
            </button>
          </div>
        )}

        <button type="button" data-focusable className={styles.item} onClick={() => setSfxEnabled(!audio.sfxEnabled)}>
          {audio.sfxEnabled ? <Volume2 size={18} /> : <VolumeX size={18} />}
          {t("controlCenter.sound")}
        </button>
        <button type="button" data-focusable className={styles.item} onClick={() => goto("profile")}>
          <User size={18} />
          {t("controlCenter.profile")}
        </button>
        <button type="button" data-focusable className={styles.item} onClick={() => goto("settings", "gamepad")}>
          <Gamepad2 size={18} />
          {t("controlCenter.controllers")}
        </button>
        <button type="button" data-focusable className={styles.item} onClick={() => goto("logs")}>
          <Terminal size={18} />
          {t("controlCenter.logs")}
        </button>
        <button type="button" data-focusable className={styles.item} onClick={() => goto("settings")}>
          <SettingsIcon size={18} />
          {t("controlCenter.settings")}
        </button>
        <button
          type="button"
          data-focusable
          className={styles.item}
          onClick={() => {
            onClose();
            onOpenPower();
          }}
        >
          <Power size={18} />
          {t("controlCenter.power")}
        </button>
      </div>
    </div>
  );
}
