import { useHomeLayout, setHomeLayout, type HomeLayout } from "../lib/homeLayout";
import { useT } from "../i18n";
import styles from "./LayoutPicker.module.css";

/** Graphical Home-layout picker: a radio dot + label + a small CSS-drawn
 * mockup of each layout, one row, two options -- same visual language as
 * GNOME's Hot Corner / Window Resize preference cards (outlined preview box,
 * accent-colored shapes), not a plain dropdown. Renders in the Background &
 * Layout Settings pane, under the theme thumbnails. */
export function LayoutPicker() {
  const active = useHomeLayout();
  const t = useT();

  // Default (library) first/leftmost.
  const options: { id: HomeLayout; label: string }[] = [
    { id: "library", label: t("settings.background.layout.library") },
    { id: "standard", label: t("settings.background.layout.standard") },
  ];

  return (
    <div style={{ display: "flex", gap: 16, flexWrap: "wrap" }}>
      {options.map((opt) => {
        const isActive = opt.id === active;
        return (
          // A plain div, not a button -- FocusNav.tsx's FOCUSABLE_SELECTOR
          // treats every <button> as a focus target regardless of
          // data-focusable, so a button here would put the ring back on
          // the whole card (preview + radio row) instead of just the
          // preview box below.
          <div key={opt.id} className={styles.card} onClick={() => setHomeLayout(opt.id)}>
            <div data-focusable className={`${styles.preview} ${isActive ? styles.previewActive : ""}`}>
              {opt.id === "standard" ? <StandardMock /> : <LibraryMock />}
            </div>
            <span className={`${styles.labelRow} ${isActive ? styles.labelRowActive : ""}`}>
              <span className={`${styles.radio} ${isActive ? styles.radioActive : ""}`}>{isActive && <span className={styles.radioDot} />}</span>
              {opt.label}
            </span>
          </div>
        );
      })}
    </div>
  );
}

/** Mock of today's layout: a row of small growing tiles, then a wide info
 * block (title + CTA) below. */
function StandardMock() {
  return (
    <>
      <div style={{ display: "flex", gap: 3, alignItems: "flex-end", height: "42%" }}>
        {[0.55, 0.55, 1, 0.55, 0.55].map((h, i) => (
          <div key={i} style={{ flex: i === 2 ? 2 : 1, height: `${h * 100}%`, borderRadius: 3, background: i === 2 ? "var(--accent)" : "var(--glass-bg-strong)" }} />
        ))}
      </div>
      <div style={{ flex: 1 }} />
      <div style={{ display: "flex", flexDirection: "column", gap: 4 }}>
        <div style={{ width: "55%", height: 6, borderRadius: 2, background: "var(--text-secondary)" }} />
        <div style={{ width: "30%", height: 9, borderRadius: 4, background: "var(--accent)" }} />
      </div>
    </>
  );
}

/** Mock of the library layout: a thin shortcut row, then a grid of bigger,
 * uniform cards filling the rest. */
function LibraryMock() {
  return (
    <>
      <div style={{ display: "flex", gap: 3, height: "18%" }}>
        {[0, 1, 2, 3].map((i) => (
          <div key={i} style={{ flex: 1, borderRadius: 2, background: "var(--glass-bg-strong)" }} />
        ))}
      </div>
      <div style={{ flex: 1, display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: 3 }}>
        {Array.from({ length: 8 }).map((_, i) => (
          <div key={i} style={{ borderRadius: 2, background: i === 0 ? "var(--accent)" : "var(--glass-bg-strong)" }} />
        ))}
      </div>
    </>
  );
}
