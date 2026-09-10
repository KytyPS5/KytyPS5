import { useEffect, useRef, useState, type ReactNode } from "react";
import { useGamepadActions } from "../nav/useGamepadActions";
import { useT } from "../i18n";
import styles from "./Boot.module.css";

type BootPhase = "black" | "logo" | "logoExit" | "atmosphere" | "homeEnter" | "ready";

/** One controller, one timer chain (04-tauri-implementation.md's boot state
 * machine) -- not scattered setTimeouts across components. Each entry's
 * `duration` is how long THAT phase holds before advancing to the next. */
const BOOT_SEQUENCE: { phase: Exclude<BootPhase, "ready">; duration: number }[] = [
  { phase: "black", duration: 150 },
  { phase: "logo", duration: 700 },
  { phase: "logoExit", duration: 500 },
  { phase: "atmosphere", duration: 800 },
  { phase: "homeEnter", duration: 500 },
];

// A slow library scan should never hang the splash indefinitely -- READY
// fires once the boot chain AND the data load have both finished, whichever
// is later, capped by this ceiling.
const DATA_READY_CEILING_MS = 6000;

/** Wraps the whole app. Children are always mounted underneath (so Home's
 * own data load and entrance stagger are already progressing by the time
 * the overlay clears) -- this only draws the BOOT_BLACK -> ... -> READY
 * overlay on top and fades it away, per 01-motion-system.md's boot sequence
 * and 04-tauri-implementation.md's boot state machine. */
export function BootController({ dataReady, children }: { dataReady: boolean; children: ReactNode }) {
  const [phase, setPhase] = useState<BootPhase>("black");
  const [chainDone, setChainDone] = useState(false);
  const skippedRef = useRef(false);
  const t = useT();

  useEffect(() => {
    let cancelled = false;
    const timers: ReturnType<typeof setTimeout>[] = [];
    let idx = 0;
    const step = () => {
      if (cancelled) return;
      if (idx >= BOOT_SEQUENCE.length) {
        setChainDone(true);
        return;
      }
      const { phase: p, duration } = BOOT_SEQUENCE[idx];
      setPhase(p);
      idx += 1;
      timers.push(setTimeout(step, duration));
    };
    step();
    return () => {
      cancelled = true;
      timers.forEach(clearTimeout);
    };
  }, []);

  // READY once the boot chain has finished AND data has loaded -- whichever
  // is later -- with a ceiling under the data-load side.
  useEffect(() => {
    if (!chainDone || skippedRef.current) return;
    if (dataReady) {
      setPhase("ready");
      return;
    }
    const ceiling = setTimeout(() => setPhase("ready"), DATA_READY_CEILING_MS);
    return () => clearTimeout(ceiling);
  }, [chainDone, dataReady]);

  // Any key or gamepad button skips straight to READY.
  const skip = () => {
    if (skippedRef.current) return;
    skippedRef.current = true;
    setPhase("ready");
  };
  useEffect(() => {
    if (phase === "ready") return;
    window.addEventListener("keydown", skip);
    window.addEventListener("pointerdown", skip);
    return () => {
      window.removeEventListener("keydown", skip);
      window.removeEventListener("pointerdown", skip);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [phase]);
  useGamepadActions(() => {
    if (phase !== "ready") skip();
  });

  return (
    <>
      {children}
      {phase !== "ready" && (
        <div className={styles.root} data-phase={phase}>
          <div className={styles.atmosphere} style={{ backgroundImage: "url(/art/ambient_boot.png)" }} />
          <img className={styles.logo} src="/art/kyty_mark.png" alt="" />
          <div className={styles.status}>{t("boot.status")}</div>
        </div>
      )}
    </>
  );
}
