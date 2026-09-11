import { useEffect, useRef, useState } from "react";
import { Check } from "lucide-react";
import type { GameStatus } from "../types";
import { useFocusNav } from "../nav/FocusNav";
import { useT } from "../i18n";
import { DEFAULT_FILTERS, type LibraryFilters, type SortId } from "../lib/librarySort";
import { Toggle } from "./Toggle";
import styles from "./LibraryFilterPanel.module.css";

const SORT_IDS: SortId[] = ["nameAsc", "nameDesc", "recent", "played", "addedNew", "addedOld"];
const STATUS_IDS: GameStatus[] = ["Unknown", "InGame", "MainMenu", "Logo", "DoesntBoot"];

type Submenu = "sort" | "status" | "played" | null;

/** Dark flyout anchored to Library's left rail (owner rule: Search + Sort/
 * filter are the rail's only two flyouts). Escape / gamepad back close only
 * this panel, via the same pushFocusScope/popFocusScope contract Modal.tsx
 * and PowerMenu.tsx already use -- see FocusNav.tsx's own doc comment.
 *
 * Each submenu (Sort by / Status / Played) gets its OWN nested focus scope,
 * pushed on top of the panel's own scope, rather than sharing one flat
 * scope for the whole flyout. That gives two things a flat scope can't:
 * the row that opens a submenu must itself be reachable by d-pad (a real
 * <button>, not a div with an onClick only a mouse can trigger), and
 * gamepad back closes just the open submenu first (restoring focus to the
 * row that opened it, via pushFocusScope's own restoreEl bookkeeping)
 * before a second back closes the whole flyout. */
export function LibraryFilterPanel({
  sort,
  onSortChange,
  filters,
  onFiltersChange,
  onClose,
}: {
  sort: SortId;
  onSortChange: (sort: SortId) => void;
  filters: LibraryFilters;
  onFiltersChange: (filters: LibraryFilters) => void;
  onClose: () => void;
}) {
  const t = useT();
  const rootRef = useRef<HTMLDivElement>(null);
  const submenuRef = useRef<HTMLDivElement>(null);
  const { pushFocusScope, popFocusScope } = useFocusNav();
  const [submenu, setSubmenu] = useState<Submenu>(null);

  useEffect(() => {
    if (!rootRef.current) return;
    pushFocusScope(rootRef.current, onClose);
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const onPointerDown = (event: PointerEvent) => {
      const target = event.target as HTMLElement | null;
      if (!target || !rootRef.current) return;
      if (rootRef.current.contains(target) || target.closest("[data-flyout-trigger]")) return;
      onClose();
    };
    document.addEventListener("pointerdown", onPointerDown);
    return () => document.removeEventListener("pointerdown", onPointerDown);
  }, [onClose]);

  useEffect(() => {
    if (!submenu || !submenuRef.current) return;
    const root = submenuRef.current;
    pushFocusScope(root, () => setSubmenu(null));
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [submenu]);

  const toggleSubmenu = (id: Exclude<Submenu, null>) => setSubmenu((current) => (current === id ? null : id));

  const toggleStatus = (status: GameStatus) => {
    const has = filters.statuses.includes(status);
    onFiltersChange({ ...filters, statuses: has ? filters.statuses.filter((s) => s !== status) : [...filters.statuses, status] });
  };

  return (
    <div ref={rootRef} className={styles.panel}>
      <button type="button" className={styles.row} onClick={() => toggleSubmenu("sort")}>
        <span>{t("library.sort.title")}</span>
        <span className={styles.rowValue}>{t(`library.sort.${sort}`)}</span>
      </button>
      {submenu === "sort" && (
        <div ref={submenuRef} className={styles.submenu}>
          {SORT_IDS.map((id) => (
            <button
              type="button"
              key={id}
              className={styles.option}
              onClick={() => {
                onSortChange(id);
                setSubmenu(null);
              }}
            >
              <span>{t(`library.sort.${id}`)}</span>
              {id === sort && <Check size={14} className={styles.check} />}
            </button>
          ))}
        </div>
      )}

      <div className={styles.subheading}>{t("library.filters.title")}</div>

      <button type="button" className={styles.row} onClick={() => toggleSubmenu("status")}>
        <span>{t("library.filters.status")}</span>
        <span className={styles.rowValue}>{filters.statuses.length === 0 ? t("library.filters.any") : filters.statuses.length}</span>
      </button>
      {submenu === "status" && (
        <div ref={submenuRef} className={styles.submenu}>
          <button type="button" className={styles.option} onClick={() => onFiltersChange({ ...filters, statuses: [] })}>
            <span>{t("library.filters.any")}</span>
            {filters.statuses.length === 0 && <Check size={14} className={styles.check} />}
          </button>
          {STATUS_IDS.map((status) => (
            <button type="button" key={status} className={styles.option} onClick={() => toggleStatus(status)}>
              <span>{t(`gameDetail.status.${status}`)}</span>
              {filters.statuses.includes(status) && <Check size={14} className={styles.check} />}
            </button>
          ))}
        </div>
      )}

      <button type="button" className={styles.row} onClick={() => toggleSubmenu("played")}>
        <span>{t("library.filters.played")}</span>
        <span className={styles.rowValue}>
          {filters.played === "all" ? t("library.filters.playedAll") : filters.played === "played" ? t("library.filters.playedYes") : t("library.filters.playedNo")}
        </span>
      </button>
      {submenu === "played" && (
        <div ref={submenuRef} className={styles.submenu}>
          {(["all", "played", "never"] as const).map((id) => (
            <button
              type="button"
              key={id}
              className={styles.option}
              onClick={() => {
                onFiltersChange({ ...filters, played: id });
                setSubmenu(null);
              }}
            >
              <span>{id === "all" ? t("library.filters.playedAll") : id === "played" ? t("library.filters.playedYes") : t("library.filters.playedNo")}</span>
              {filters.played === id && <Check size={14} className={styles.check} />}
            </button>
          ))}
        </div>
      )}

      <div className={styles.row}>
        <span>{t("library.filters.customSettings")}</span>
        <Toggle
          ariaLabel={t("library.filters.customSettings")}
          checked={filters.customOnly}
          onChange={(v) => onFiltersChange({ ...filters, customOnly: v })}
        />
      </div>

      <button type="button" className="pill-button" onClick={() => onFiltersChange(DEFAULT_FILTERS)}>
        {t("library.filters.reset")}
      </button>
    </div>
  );
}
