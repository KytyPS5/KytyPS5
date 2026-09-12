import { useEffect, useRef } from "react";
import { useGamepadActions } from "../nav/useGamepadActions";
import { useT } from "../i18n";
import { useOverlayLifecycle } from "./useOverlayLifecycle";
import styles from "./RestMode.module.css";

/** A screen state, not a system suspend -- copy and behavior both say so.
 * No focusable rows and no focus scope: "any input reverses it" means a
 * plain global listener is enough, there is nothing inside to navigate.
 * AppShell's own `dimmed` prop (see shell/AppShell.module.css) handles the
 * "home content recedes" half of the sequence; this component only owns the
 * atmospheric scene and status line that becomes dominant over it. */
export function RestMode({ open, onClose }: { open: boolean; onClose: () => void }) {
  const { mounted, isOpen } = useOverlayLifecycle(open, 700);
  const t = useT();
  const wokeRef = useRef(false);

  useEffect(() => {
    wokeRef.current = false;
  }, [open]);

  const wake = () => {
    if (wokeRef.current) return;
    wokeRef.current = true;
    onClose();
  };

  useEffect(() => {
    if (!open) return;
    window.addEventListener("keydown", wake);
    window.addEventListener("pointerdown", wake);
    return () => {
      window.removeEventListener("keydown", wake);
      window.removeEventListener("pointerdown", wake);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);

  useGamepadActions(() => {
    if (open) wake();
  });

  if (!mounted) return null;

  return (
    <div className={`${styles.root} ${isOpen ? styles.isOpen : ""}`}>
      <div className={styles.atmosphere} style={{ backgroundImage: "url(/art/ambient_idle.webp)" }} />
      <div className={styles.status}>{t("restMode.status")}</div>
    </div>
  );
}
