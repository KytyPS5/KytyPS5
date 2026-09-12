// Kyty Launcher, author: Hyphaed.
import { useCallback, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { AppShell } from "./shell/AppShell";
import { BootController } from "./boot/BootController";
import { ControlCenter } from "./overlays/ControlCenter";
import { PowerMenu } from "./overlays/PowerMenu";
import { RestMode } from "./overlays/RestMode";
import { FocusNavProvider } from "./nav/FocusNav";
import { HomeView } from "./views/Home";
import { LibraryView } from "./views/Library";
import { SettingsView } from "./views/Settings";
import { LogsView } from "./views/Logs";
import { ProfileView } from "./views/Profile";
import { ExitConfirmView } from "./views/ExitConfirm";
import { loadPrefs, refreshLibrary } from "./store/library";
import { ensureRunListeners, syncRunningState } from "./store/run";
import { refreshPlayHistory } from "./store/playtime";
import { playSfx, warmSfx } from "./lib/sfx";
import { reapplyStoredOutputSink } from "./lib/audioSettings";
import { applyStoredDisplayModeAtBoot } from "./lib/displayMode";
import { applyStoredUiScaleAtBoot } from "./lib/uiScale";
import { ensureDefaultProfile } from "./lib/profiles";

// "trophies" is not a ViewId: GameDetail's Trophies button renders
// TrophiesView inline as one of its own tabs (see components/GameDetail.tsx)
// instead of navigating the whole app away from Library, so there is no
// standalone trophies route to switch to any more.
export type ViewId = "home" | "library" | "settings" | "logs" | "profile" | "exit";

export default function App() {
  const [view, setView] = useState<ViewId>("home");
  const [selectedGamePath, setSelectedGamePath] = useState<string | null>(null);
  // Which view the game-detail pane was opened from. The pane itself always
  // lives inside Library, but Home's "View details" jumps straight into it
  // without the user ever seeing the Library grid -- so backing out of it
  // has to return to Home, not strand them in a grid they never opened.
  const [detailOrigin, setDetailOrigin] = useState<ViewId>("library");
  const [dataReady, setDataReady] = useState(false);
  // null = not yet known. BootController reads this only once, at mount
  // (its own useState initializer), so nothing renders below until this
  // resolves -- otherwise a resumed launch would still flash the boot
  // animation for the one frame before the answer came back.
  const [skipBootIntro, setSkipBootIntro] = useState<boolean | null>(null);
  const [controlCenterOpen, setControlCenterOpen] = useState(false);
  const [powerMenuOpen, setPowerMenuOpen] = useState(false);
  const [restMode, setRestMode] = useState(false);

  useEffect(() => {
    void invoke<boolean>("is_resumed_launch")
      .then(setSkipBootIntro)
      .catch(() => setSkipBootIntro(false));
    warmSfx();
    playSfx("boot");
    void reapplyStoredOutputSink();
    applyStoredDisplayModeAtBoot();
    applyStoredUiScaleAtBoot();
    void (async () => {
      await loadPrefs();
      await refreshLibrary();
      // configStore is guaranteed populated at this point -- see
      // ensureDefaultProfile's own doc comment for why that matters.
      ensureDefaultProfile();
      await refreshPlayHistory();
      await ensureRunListeners();
      await syncRunningState();
      setDataReady(true);
    })();
  }, []);

  // "back" (was CIRCLE): a real hierarchical "go up one level", not a fixed
  // destination -- Home is the hub every other view branches off of
  // (GameDetail's Settings/Patches/Saved-data tabs are inline panes, not
  // modals, so there is nothing at the app level for "back" to close first).
  // From Library with a game selected, back closes the detail pane before
  // leaving the view at all, mirroring the real PS5's own "circle backs out
  // one level at a time" behavior.
  const handleBack = useCallback(() => {
    if (view === "library" && selectedGamePath) {
      setSelectedGamePath(null);
      // Opened from Home, so the grid was never a step the user took --
      // going "up one level" from here means Home itself.
      if (detailOrigin === "home") setView("home");
      return;
    }
    if (view !== "home") setView("home");
  }, [view, selectedGamePath, detailOrigin]);

  // "menu" (was TRIANGLE): opens the control center overlay over the live
  // scene (ps5_tauri_ui_guidelines/03's "control-center / quick-menu
  // overlay") -- previously this just returned to Home, which is now one
  // click away inside the overlay instead (its Settings/Logs/Profile rows).
  const handleMenu = useCallback(() => {
    setControlCenterOpen(true);
  }, []);

  // Keyboard equivalent of the gamepad menu button -- FocusNav's own
  // keydown fallback only claims arrows/Enter/Escape, so this is a
  // dedicated app-level listener rather than routed through it.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "F1") {
        e.preventDefault();
        setControlCenterOpen(true);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  // tauri.conf.json's window backgroundColor is the same near-black as
  // BootController's own first phase, so this brief gap (waiting to learn
  // whether to skip the intro) reads as more black, not a flash of
  // unstyled content.
  if (skipBootIntro === null) return null;

  return (
    <BootController dataReady={dataReady} skipIntro={skipBootIntro}>
      <FocusNavProvider resetKey={view} onBack={handleBack} onMenu={handleMenu}>
        <AppShell view={view} onNavigate={setView} onBack={handleBack} dimmed={restMode}>
          {view === "home" && (
            <HomeView
              onNavigate={setView}
              onViewDetails={(path) => {
                setSelectedGamePath(path);
                setDetailOrigin("home");
                setView("library");
              }}
            />
          )}
          {view === "library" && (
            <LibraryView
              selectedGamePath={selectedGamePath}
              onSelectGame={(path) => {
                // Picked out of the grid, so the grid is where back returns.
                setDetailOrigin("library");
                setSelectedGamePath(path);
              }}
              onNavigate={setView}
            />
          )}
          {view === "settings" && <SettingsView />}
          {view === "logs" && <LogsView />}
          {view === "profile" && <ProfileView onNavigate={setView} />}
          {view === "exit" && <ExitConfirmView onNavigate={setView} />}
        </AppShell>

        <ControlCenter
          open={controlCenterOpen}
          onClose={() => setControlCenterOpen(false)}
          onNavigate={setView}
          onOpenPower={() => setPowerMenuOpen(true)}
        />
        <PowerMenu
          open={powerMenuOpen}
          onClose={() => setPowerMenuOpen(false)}
          onEnterRestMode={() => setRestMode(true)}
        />
      </FocusNavProvider>
      <RestMode open={restMode} onClose={() => setRestMode(false)} />
    </BootController>
  );
}
