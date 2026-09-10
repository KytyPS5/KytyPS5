import { useEffect, useRef } from "react";
import { Moon, Power, X } from "lucide-react";
import { useWindowControls } from "../shell/useWindowControls";
import { useFocusNav } from "../nav/FocusNav";
import { useT } from "../i18n";
import overlayStyles from "./Overlay.module.css";
import { useOverlayLifecycle } from "./useOverlayLifecycle";
import styles from "./PowerMenu.module.css";

/** Compact floating surface (03-components-and-layout.md's context-menu
 * recipe), entering as one coherent unit -- items never fly in individually.
 * Focus moves only the active row. */
export function PowerMenu({ open, onClose, onEnterRestMode }: { open: boolean; onClose: () => void; onEnterRestMode: () => void }) {
  const { mounted, isOpen } = useOverlayLifecycle(open, 280);
  const rootRef = useRef<HTMLDivElement>(null);
  const { pushFocusScope, popFocusScope } = useFocusNav();
  const { close } = useWindowControls();
  const t = useT();

  useEffect(() => {
    if (!open || !rootRef.current) return;
    pushFocusScope(rootRef.current, onClose);
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);

  if (!mounted) return null;

  return (
    <div className={`${overlayStyles.backdrop} ${isOpen ? overlayStyles.isOpen : ""}`} onClick={onClose}>
      <div ref={rootRef} className={`${styles.panel} ${isOpen ? styles.panelOpen : ""}`} onClick={(e) => e.stopPropagation()}>
        <div className={styles.title}>{t("power.title")}</div>
        <button
          type="button"
          data-focusable
          className={styles.item}
          onClick={() => {
            onClose();
            onEnterRestMode();
          }}
        >
          <Moon size={17} />
          {t("power.rest")}
        </button>
        <button type="button" data-focusable className={styles.item} onClick={() => void close()}>
          <Power size={17} />
          {t("power.quit")}
        </button>
        <button type="button" data-focusable className={styles.item} onClick={onClose}>
          <X size={17} />
          {t("power.cancel")}
        </button>
      </div>
    </div>
  );
}
