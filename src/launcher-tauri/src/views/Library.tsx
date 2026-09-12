import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { ListFilter, Search } from "lucide-react";
import type { ViewId } from "../App";
import type { CompatibilityMap } from "../types";
import { useStore } from "../store/observable";
import { gamesStore, libraryErrorStore, libraryLoadingStore } from "../store/library";
import { runGame } from "../store/run";
import { playHistoryStore } from "../store/playtime";
import { GameCard } from "../components/GameCard";
import { GameDetail } from "../components/GameDetail";
import { LibraryFilterPanel } from "../components/LibraryFilterPanel";
import { applyLibraryView, hasActiveFilters, useLibraryFilters, useLibrarySort, setLibraryFilters, setLibrarySort } from "../lib/librarySort";
import { requestSettingsCategory } from "../lib/settingsNav";
import { useFocusNav } from "../nav/FocusNav";
import { useT } from "../i18n";
import styles from "./Library.module.css";

export function LibraryView({
  selectedGamePath,
  onSelectGame,
  onNavigate,
}: {
  selectedGamePath: string | null;
  onSelectGame: (path: string | null) => void;
  onNavigate: (v: ViewId) => void;
}) {
  const games = useStore(gamesStore);
  const loading = useStore(libraryLoadingStore);
  const error = useStore(libraryErrorStore);
  const history = useStore(playHistoryStore);
  const sort = useLibrarySort();
  const filters = useLibraryFilters();
  const t = useT();
  const { focusedEl, pushFocusScope, popFocusScope } = useFocusNav();

  const [searchOpen, setSearchOpen] = useState(false);
  const [filterOpen, setFilterOpen] = useState(false);
  const [query, setQuery] = useState("");
  const [compatibility, setCompatibility] = useState<CompatibilityMap>({});
  const [compatibilityIsLocal, setCompatibilityIsLocal] = useState(false);
  // Which tile currently "names itself" (PS5 reference: only the browsed
  // tile shows a caption) -- driven by mouse hover or gamepad/keyboard
  // focus, same pattern Home.tsx uses for its own library-layout grid.
  // Independent of `selected` below: browsing never opens anything, only
  // a click/confirm does.
  const [highlighted, setHighlighted] = useState<string | null>(null);
  const detailRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    void (async () => {
      setCompatibilityIsLocal(await invoke<boolean>("compatibility_is_local"));
      setCompatibility(await invoke<CompatibilityMap>("compatibility_snapshot"));
      try {
        setCompatibility(await invoke<CompatibilityMap>("compatibility_refresh"));
      } catch {
        // Remote fetch failed, keep whatever local/cached snapshot we had.
      }
    })();
  }, []);

  const rescanCompatibility = async () => {
    setCompatibility(await invoke<CompatibilityMap>("compatibility_snapshot"));
  };

  const filtered = useMemo(
    () => applyLibraryView(games, history, compatibility, query, filters, sort),
    [games, history, compatibility, query, filters, sort],
  );

  // Stable per-card handlers, paired with GameCard's own React.memo -- see
  // its doc comment for why both halves matter together. Keyed by gamePath
  // rather than array index since `filtered` (unlike Home's `games`) can
  // reorder/exclude entries as sort/filter state changes independent of
  // `highlighted`. Rebuilt only when `filtered` or `onSelectGame` actually
  // change, not on every hover.
  const cardHandlers = useMemo(
    () =>
      new Map(
        filtered.map((g) => [
          g.config.gamePath,
          {
            onClick: () => onSelectGame(g.config.gamePath),
            onDoubleClick: () => void runGame(g.config, g.config.titleId),
            onContextMenu: (e: React.MouseEvent) => {
              e.preventDefault();
              onSelectGame(g.config.gamePath);
            },
            onMouseEnter: () => setHighlighted(g.config.gamePath),
          },
        ]),
      ),
    [filtered, onSelectGame],
  );

  const selected = games.find((g) => g.config.gamePath === selectedGamePath) ?? null;
  const viewActive = query.trim() !== "" || hasActiveFilters(filters);

  useEffect(() => {
    const key = focusedEl?.dataset.focusKey;
    if (!key || !key.startsWith("card-")) return;
    setHighlighted(key.slice("card-".length));
  }, [focusedEl]);

  // A game takes over the whole screen (not a docked side panel): opening
  // one replaces the grid entirely, so it gets its own focus scope --
  // auto-focuses its first control (Run) on open, and back/Escape returns
  // to the grid via App.tsx's own "library + selectedGamePath" back rule
  // (see App.tsx's handleBack) rather than this scope's onEscape needing to
  // duplicate that logic.
  useEffect(() => {
    if (!selected || !detailRef.current) return;
    const root = detailRef.current;
    pushFocusScope(root, () => onSelectGame(null));
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selected?.config.gamePath]);

  const goToFolders = () => {
    requestSettingsCategory("folders");
    onNavigate("settings");
  };

  if (selected) {
    // GameDetail owns its own full page chrome (.page/.scrim/header/rail),
    // matching Library's and Settings' -- this wrapper only exists for the
    // focus-scope ref and the entrance animation.
    return (
      <div ref={detailRef} className={styles.fullDetail}>
        <GameDetail
          game={selected}
          compatibility={compatibility}
          compatibilityIsLocal={compatibilityIsLocal}
          onRescanCompatibility={rescanCompatibility}
        />
      </div>
    );
  }

  return (
    <div className={styles.page}>
      <div className={styles.scrim} />
      <div className={styles.body}>
        {/* Bare text title, no icon (owner rule, 2026-09-09: matches the
           real PS5 Settings reference -- a page title never pairs with an
           icon; only rail/list items do). */}
        <h1 className={styles.title}>{t("library.title")}</h1>

        <div className={styles.content}>
          <nav className={styles.rail}>
            <button
              type="button"
              data-focusable
              data-focus-key="library-rail-search"
              className={`icon-button ${searchOpen || query ? "active" : ""}`}
              title={t("library.searchButton")}
              aria-label={t("library.searchButton")}
              onClick={() => setSearchOpen((v) => !v)}
            >
              <Search size={18} strokeWidth={1.8} />
            </button>
            <button
              type="button"
              data-focusable
              data-focus-key="library-rail-filter"
              data-flyout-trigger
              className={`icon-button ${filterOpen || hasActiveFilters(filters) ? "active" : ""}`}
              title={t("library.filters.button")}
              aria-label={t("library.filters.button")}
              onClick={() => setFilterOpen((v) => !v)}
            >
              <ListFilter size={18} strokeWidth={1.8} />
            </button>
            {filterOpen && (
              <LibraryFilterPanel
                sort={sort}
                onSortChange={setLibrarySort}
                filters={filters}
                onFiltersChange={setLibraryFilters}
                onClose={() => setFilterOpen(false)}
              />
            )}
          </nav>

          <div className={styles.main}>
            <div className={styles.countRow}>
              {searchOpen ? (
                <input
                  type="search"
                  autoFocus
                  className={styles.searchField}
                  placeholder={t("library.searchPlaceholder")}
                  aria-label={t("library.searchButton")}
                  value={query}
                  onChange={(e) => setQuery(e.target.value)}
                />
              ) : (
                <span className={styles.countLabel}>
                  {viewActive ? t("library.showing", { count: filtered.length, total: games.length }) : t("library.all", { count: games.length })}
                </span>
              )}
              {/* Read-only summary, not a second way to open the flyout --
                 the rail's Sort/Filter icon is the only control for that
                 now (owner rule, 2026-09-09). Not focusable and does
                 nothing on click. */}
              <span className={styles.sortButton}>{t("library.sortByLabel", { value: t(`library.sort.${sort}`) })}</span>
            </div>

            {loading && <p className={styles.hint}>{t("library.scanning")}</p>}
            {error && <p className={styles.hint} style={{ color: "var(--error)" }}>{error}</p>}

            {!loading && !error && games.length === 0 && (
              <EmptyState onGoToFolders={goToFolders} />
            )}

            {!loading && !error && games.length > 0 && filtered.length === 0 && (
              <p className={styles.hint}>{t("library.noResults")}</p>
            )}

            {!loading && filtered.length > 0 && (
              <div className={styles.grid} data-scroll-region>
                {filtered.map((g) => (
                  <GameCard
                    key={g.config.gamePath}
                    game={g}
                    selected={highlighted === g.config.gamePath}
                    status={compatibility[g.config.titleId.toUpperCase()]?.status}
                    captionMode="selected"
                    {...cardHandlers.get(g.config.gamePath)!}
                  />
                ))}
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

function EmptyState({ onGoToFolders }: { onGoToFolders: () => void }) {
  const t = useT();
  return (
    <div className={`glass-panel ${styles.emptyState}`}>
      <p style={{ margin: 0, fontSize: 14, color: "var(--text-secondary)" }}>{t("library.emptyHint")}</p>
      <button type="button" className="pill-button primary" onClick={onGoToFolders}>
        {t("library.emptyAction")}
      </button>
    </div>
  );
}
