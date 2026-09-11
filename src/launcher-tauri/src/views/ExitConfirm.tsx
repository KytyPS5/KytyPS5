import { getCurrentWindow } from "@tauri-apps/api/window";
import { GlassPanel } from "../components/GlassPanel";
import { useT } from "../i18n";
import type { ViewId } from "../App";
import styles from "./ExitConfirm.module.css";

/** Confirmation page for the fullscreen-only Exit Launcher icon in TopBar
 * (shell/TopBar.tsx) -- fullscreen has no window-chrome close button to
 * reach by mistake, but this icon sits in that same top-right slot, so a
 * confirm step guards the stray click the window-chrome button never
 * needed one for. Settings > System's own "Exit Kyty Launcher" button
 * still closes immediately -- that one is a few clicks deep in a settings
 * list, not a single tap in the corner of the screen. */
export function ExitConfirmView({ onNavigate }: { onNavigate: (v: ViewId) => void }) {
  const t = useT();

  return (
    <div style={{ position: "relative", height: "100%", overflow: "hidden" }}>
      <div className={styles.scrim} />
      <div className={styles.center}>
        <GlassPanel variant="heavy" className={styles.panel}>
          {/* Same title style as every other page (pageHeader.module.css's
             font-display, e.g. Library.module.css's "Game Library") -- no
             icon chip here, text carries the whole thing. */}
          <h1 className={styles.title}>{t("exitConfirm.title")}</h1>
          <div className={styles.actions}>
            <button type="button" className="pill-button" onClick={() => onNavigate("home")}>
              {t("common.cancel")}
            </button>
            <button type="button" className="pill-button" onClick={() => void getCurrentWindow().close()}>
              {t("exitConfirm.confirm")}
            </button>
          </div>
        </GlassPanel>
      </div>
    </div>
  );
}
