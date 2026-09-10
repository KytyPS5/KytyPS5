import { useEffect, useRef, useState, type ReactNode } from "react";
import { useT } from "../i18n";
import { useFocusNav } from "../nav/FocusNav";
import styles from "./Modal.module.css";

export function Modal({
  title,
  onClose,
  width = 560,
  children,
  footer,
}: {
  title: string;
  onClose: () => void;
  width?: number;
  children: ReactNode;
  footer?: ReactNode;
}) {
  const t = useT();
  const surfaceRef = useRef<HTMLDivElement>(null);
  const { pushFocusScope, popFocusScope } = useFocusNav();

  // Entrance only (01-motion-system.md's overlay values) -- callers render
  // this conditionally (`{show && <Modal .../>}`) rather than passing a
  // controlled `open`, so there is no later "closing" render to animate an
  // exit from; only the entrance gets the rAF-deferred fade + rise.
  const [visible, setVisible] = useState(false);
  useEffect(() => {
    const raf = requestAnimationFrame(() => setVisible(true));
    return () => cancelAnimationFrame(raf);
  }, []);

  // Focus scope: Escape closes this modal instead of FocusNav's normal
  // back/onBack chain (previously a real bug -- Escape over an open Modal
  // navigated the whole app to Home instead of closing it, since Modal had
  // no Escape handling of its own).
  useEffect(() => {
    if (!surfaceRef.current) return;
    pushFocusScope(surfaceRef.current, onClose);
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <div
      onClick={onClose}
      style={{
        position: "fixed",
        inset: 0,
        zIndex: "var(--z-modal)",
        background: "var(--backdrop-scrim)",
        backdropFilter: "blur(4px)",
        opacity: visible ? 1 : 0,
        transition: "opacity 200ms var(--ease-ui)",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
      }}
    >
      <div
        ref={surfaceRef}
        role="dialog"
        aria-modal="true"
        aria-label={title}
        onClick={(e) => e.stopPropagation()}
        className={styles.surface}
        style={{
          width,
          maxWidth: "92vw",
          maxHeight: "86vh",
          display: "flex",
          flexDirection: "column",
          overflow: "hidden",
          opacity: visible ? 1 : 0,
          transform: visible ? "translateY(0) scale(1)" : "translateY(12px) scale(0.985)",
          transition: "opacity 240ms var(--ease-enter), transform 280ms var(--ease-enter)",
        }}
      >
        <div
          className={styles.header}
          style={{
            display: "flex",
            alignItems: "center",
            justifyContent: "space-between",
            padding: "16px 20px",
            borderBottom: "1px solid",
          }}
        >
          <h2 style={{ margin: 0, fontSize: 15, fontWeight: 700 }}>{title}</h2>
          <button className="icon-button" onClick={onClose} aria-label={t("common.close")}>
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none">
              <path d="M6 6l12 12M18 6 6 18" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
            </svg>
          </button>
        </div>
        <div data-scroll-region style={{ padding: 20, overflowY: "auto", flex: 1 }}>{children}</div>
        {footer && (
          <div
            className={styles.footer}
            style={{
              padding: "14px 20px",
              borderTop: "1px solid",
              display: "flex",
              justifyContent: "flex-end",
              gap: 10,
            }}
          >
            {footer}
          </div>
        )}
      </div>
    </div>
  );
}
