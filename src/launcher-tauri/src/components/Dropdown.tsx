// A glassmorphic replacement for native <select>. WebKitGTK (Tauri's Linux
// renderer) draws a native <select>'s open option list itself -- no CSS can
// touch it, so it can never match the app's glass surfaces (theme.css's
// `select` rule only ever reached the closed control). This renders its own
// trigger + option panel, both real styled DOM, at the cost of owning
// open/close and keyboard nav itself.
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { Check, ChevronDown } from "lucide-react";
import styles from "./Dropdown.module.css";

export interface DropdownOption {
  value: string;
  label: string;
  disabled?: boolean;
}

export function Dropdown({
  value,
  onChange,
  options,
  ariaLabel,
  disabled,
}: {
  value: string;
  onChange: (value: string) => void;
  options: DropdownOption[];
  ariaLabel?: string;
  disabled?: boolean;
}) {
  const [open, setOpen] = useState(false);
  const [rect, setRect] = useState<{ left: number; top: number; width: number; openUp: boolean } | null>(null);
  const [visible, setVisible] = useState(false);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const panelRef = useRef<HTMLDivElement>(null);

  // Entrance only, same reasoning as Modal.tsx: the panel mounts/unmounts
  // with `open` rather than a separately controlled visible flag, so only
  // the entrance gets the rAF-deferred fade + rise.
  useEffect(() => {
    if (!open) {
      setVisible(false);
      return;
    }
    const raf = requestAnimationFrame(() => setVisible(true));
    return () => cancelAnimationFrame(raf);
  }, [open]);

  const selected = options.find((o) => o.value === value);

  const place = () => {
    const el = triggerRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const maxPanel = 280;
    const spaceBelow = window.innerHeight - r.bottom;
    const openUp = spaceBelow < Math.min(maxPanel, 160) && r.top > spaceBelow;
    setRect({ left: r.left, top: openUp ? r.top : r.bottom, width: r.width, openUp });
  };

  useLayoutEffect(() => {
    if (open) place();
  }, [open]);

  useEffect(() => {
    if (!open) return;
    const onScrollOrResize = () => place();
    const onPointerDown = (e: MouseEvent) => {
      const target = e.target as Node;
      if (triggerRef.current?.contains(target) || panelRef.current?.contains(target)) return;
      setOpen(false);
    };
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation();
        setOpen(false);
        triggerRef.current?.focus();
      }
    };
    window.addEventListener("resize", onScrollOrResize);
    window.addEventListener("scroll", onScrollOrResize, true);
    window.addEventListener("mousedown", onPointerDown, true);
    window.addEventListener("keydown", onKeyDown, true);
    return () => {
      window.removeEventListener("resize", onScrollOrResize);
      window.removeEventListener("scroll", onScrollOrResize, true);
      window.removeEventListener("mousedown", onPointerDown, true);
      window.removeEventListener("keydown", onKeyDown, true);
    };
  }, [open]);

  const commit = (v: string) => {
    onChange(v);
    setOpen(false);
    triggerRef.current?.focus();
  };

  const onTriggerKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "Enter" || e.key === " " || e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      setOpen(true);
    }
  };

  const onOptionKeyDown = (e: React.KeyboardEvent, index: number) => {
    const enabled = options.map((o, i) => ({ o, i })).filter((x) => !x.o.disabled);
    const pos = enabled.findIndex((x) => x.i === index);
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      const dir = e.key === "ArrowDown" ? 1 : -1;
      const next = enabled[(pos + dir + enabled.length) % enabled.length];
      const el = panelRef.current?.querySelectorAll<HTMLButtonElement>("[data-option]")[next.i];
      el?.focus();
    } else if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      if (!options[index].disabled) commit(options[index].value);
    }
  };

  return (
    <>
      <button
        type="button"
        ref={triggerRef}
        className={`${styles.trigger} ${open ? styles.triggerOpen : ""}`}
        disabled={disabled}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={ariaLabel}
        data-focusable
        onClick={() => setOpen((v) => !v)}
        onKeyDown={onTriggerKeyDown}
      >
        <span className={styles.triggerLabel}>{selected?.label ?? ""}</span>
        <ChevronDown size={15} className={styles.chevron} />
      </button>
      {open &&
        rect &&
        createPortal(
          <div
            ref={panelRef}
            role="listbox"
            className={`${styles.panel} ${visible ? styles.panelOpen : ""}`}
            style={{
              left: rect.left,
              width: rect.width,
              maxHeight: 280,
              ...(rect.openUp ? { bottom: window.innerHeight - rect.top, top: "auto" } : { top: rect.top }),
            }}
          >
            {options.map((o, i) => (
              <button
                type="button"
                key={o.value}
                data-option
                role="option"
                aria-selected={o.value === value}
                disabled={o.disabled}
                className={`${styles.option} ${o.value === value ? styles.optionSelected : ""} ${o.disabled ? styles.optionDisabled : ""}`}
                onClick={() => !o.disabled && commit(o.value)}
                onKeyDown={(e) => onOptionKeyDown(e, i)}
                autoFocus={o.value === value}
              >
                <span>{o.label}</span>
                {o.value === value && <Check size={14} className={styles.check} />}
              </button>
            ))}
          </div>,
          document.body,
        )}
    </>
  );
}
