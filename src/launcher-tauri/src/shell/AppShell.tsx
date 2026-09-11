import { useEffect, useState, type ReactNode } from "react";
import { TopBar } from "./TopBar";
import { useWindowControls } from "./useWindowControls";
import type { ResizeDirection } from "./useWindowControls";
import type { ViewId } from "../App";
import { HeroBackground } from "../components/HeroBackground";
import { useBackground } from "../store/background";
import styles from "./AppShell.module.css";

export function AppShell({
  view,
  onNavigate,
  onBack,
  dimmed,
  children,
}: {
  view: ViewId;
  onNavigate: (v: ViewId) => void;
  /** TopBar's chevron uses this instead of a fixed onNavigate("home") --
   * App.tsx's handleBack already knows the one hierarchical exception
   * (Library with a game selected steps back to the grid, not Home), so
   * the chevron and the gamepad/keyboard back action agree instead of the
   * chevron always jumping past that level. */
  onBack: () => void;
  /** Rest mode (src/overlays/RestMode.tsx): the home scene recedes rather
   * than the overlay simply painting over it -- 01-motion-system.md's rest
   * sequence names this explicitly ("home darkens", "content recedes"). */
  dimmed?: boolean;
  children: ReactNode;
}) {
  const { startResizeDragging } = useWindowControls();
  const background = useBackground();

  // Enter-only page transition (04-tauri-implementation.md's EntranceState):
  // views used to swap by instant mount/unmount. Re-armed on every `view`
  // change via the rAF deferral pattern from that doc -- CSS does the actual
  // animating off the data-entrance attribute (AppShell.module.css), Home's
  // own module stacks a stagger on top of it for its children.
  const [entrance, setEntrance] = useState<"entering" | "visible">("entering");
  useEffect(() => {
    setEntrance("entering");
    const raf = requestAnimationFrame(() => setEntrance("visible"));
    return () => cancelAnimationFrame(raf);
  }, [view]);

  return (
    <div className={styles.shell} data-dimmed={dimmed ? "true" : "false"}>
      <ResizeGrips onGrip={startResizeDragging} />

      <div className={styles.main}>
        <TopBar view={view} onNavigate={onNavigate} onBack={onBack} />
        <div className={styles.content}>
          {/* One shell-level background, mounted once and never unmounted
             across navigation -- views used to each own a HeroBackground
             instance, which meant its crossfade state (and the Ken Burns
             pan) reset to black on every view switch (real bug,
             2026-09-09). Home publishes the active game's own art into
             store/background.ts; every other view sees heroUrl null and
             falls back to the selected dashboard background. */}
          <HeroBackground baseUrl={background.baseUrl} heroUrl={background.heroUrl} accent={background.accent} />
          <div className={styles.viewSlot} data-entrance={entrance}>
            {children}
          </div>
        </div>
      </div>
    </div>
  );
}

function ResizeGrips({ onGrip }: { onGrip: (dir: ResizeDirection) => void }) {
  const grip = (cls: string, dir: ResizeDirection) => <div className={cls} onMouseDown={() => onGrip(dir)} />;
  return (
    <>
      {grip(styles.gripN, "North")}
      {grip(styles.gripS, "South")}
      {grip(styles.gripE, "East")}
      {grip(styles.gripW, "West")}
      {grip(styles.gripNE, "NorthEast")}
      {grip(styles.gripNW, "NorthWest")}
      {grip(styles.gripSE, "SouthEast")}
      {grip(styles.gripSW, "SouthWest")}
    </>
  );
}
