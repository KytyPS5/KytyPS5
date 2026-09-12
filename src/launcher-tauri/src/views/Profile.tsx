import { useMemo } from "react";
import { Settings as SettingsIcon, Trophy } from "lucide-react";
import { useStore } from "../store/observable";
import { gamesStore } from "../store/library";
import { playHistoryStore, recentlyPlayed, mostPlayed, formatPlaytime } from "../store/playtime";
import { useLibraryTrophyCounts, useLibraryEarned, trophyTotal, type TrophyCounts } from "../store/trophies";
import { useGameArt } from "../components/GameCard";
import { useActiveProfile } from "../lib/profiles";
import { requestSettingsCategory } from "../lib/settingsNav";
import { useT, LANGUAGES } from "../i18n";
import type { GameEntry } from "../types";
import type { ViewId } from "../App";
import styles from "./Profile.module.css";

/** The identity/overview page -- who's playing, real local stats, recent
 * games. Profile *management* (create/rename/delete/switch, language,
 * gamepad) lives in Settings > Profile (ProfileCategory in views/Settings.tsx)
 * rather than being duplicated here; the "Manage profiles" button below
 * jumps straight there. */
export function ProfileView({ onNavigate }: { onNavigate: (v: ViewId) => void }) {
  const active = useActiveProfile();
  const games = useStore(gamesStore);
  const history = useStore(playHistoryStore);
  const t = useT();

  const recent = useMemo(() => recentlyPlayed(games, history, 8), [games, history]);
  const topPlayed = useMemo(() => mostPlayed(games, history, 3), [games, history]);

  const trophyCounts = useLibraryTrophyCounts(games);
  const trophyTotalCount = trophyTotal(trophyCounts);
  const trophyEarned = useLibraryEarned(games);
  // total > 0 guards the division; a library with no trophy packs at all
  // reads as 0%, not NaN%.
  const trophyPercent = trophyTotalCount > 0 ? Math.round((trophyEarned / trophyTotalCount) * 100) : 0;

  const locale = active ? (LANGUAGES.find((l) => l.code === active.localeCode) ?? LANGUAGES[0]) : LANGUAGES[0];

  return (
    <div style={{ position: "relative", height: "100%", overflow: "hidden" }}>
      {/* Always the chosen dashboard background (Settings > Dashboard
         background), like every other non-Home view -- Profile is an
         identity/overview page, not a per-game view, so it should never
         switch to a recent game's own hero art. AppShell owns the one
         shell-level HeroBackground now; Home is the only view that ever
         publishes a hero override into store/background.ts. */}
      <div
        style={{
          position: "absolute",
          inset: 0,
          background: "linear-gradient(105deg, rgba(5, 8, 18, 0.97) 0%, rgba(5, 8, 18, 0.82) 38%, rgba(5, 8, 18, 0.45) 65%, rgba(5, 8, 18, 0.15) 100%)",
        }}
      />
      <div style={{ position: "absolute", inset: 0, background: "linear-gradient(to top, rgba(5, 8, 18, 1) 0%, rgba(5, 8, 18, 0.55) 28%, transparent 55%)" }} />

      <div style={{ position: "relative", zIndex: 1, height: "100%", overflowY: "auto" }} data-scroll-region>
        <div style={{ padding: "48px 56px 0" }}>
          <div style={{ display: "flex", alignItems: "center", gap: 22, marginBottom: 28 }}>
            <div
              style={{
                width: 84,
                height: 84,
                borderRadius: "50%",
                flex: "none",
                display: "flex",
                alignItems: "center",
                justifyContent: "center",
                background: "var(--glass-bg-strong)",
                color: "var(--text-primary)",
                fontFamily: "var(--font-display)",
                fontWeight: 800,
                fontSize: 32,
                boxShadow: "0 8px 24px -6px rgba(0,0,0,0.6)",
              }}
            >
              {(active?.name ?? "?").slice(0, 1).toUpperCase()}
            </div>
            <div>
              <h1 style={{ margin: 0, fontSize: 40, fontFamily: "var(--font-display)", fontWeight: 800, color: "var(--text-primary)", lineHeight: 1.05 }}>
                {active?.name ?? "—"}
              </h1>
              <div style={{ marginTop: 6, fontSize: 13, color: "var(--text-secondary)" }}>{locale.endonym}</div>
            </div>
            <button
              type="button"
              className="pill-button"
              style={{ marginLeft: "auto" }}
              onClick={() => {
                requestSettingsCategory("profile");
                onNavigate("settings");
              }}
            >
              <SettingsIcon size={14} style={{ marginRight: 6 }} />
              {t("profileView.manage")}
            </button>
          </div>

          {/* Three flat stat cards, matching the real PS5 profile page's
             Overview (see Profile.module.css's doc for why it's three, not
             the reference's four -- no Friends concept here). Replaces the
             previous Games/Played/Playtime number strip; those two other
             numbers (games played, total playtime) have no place in this
             layout and are dropped, not relocated. */}
          <div className={styles.cardRow}>
            <div className={styles.card}>
              <div>
                <div className={styles.trophyHeader}>
                  <Trophy size={22} className={styles.trophyIcon} />
                  <span className={styles.trophyEarnedTotal}>{trophyEarned}</span>
                  <span className={styles.trophyPercent}>{trophyPercent}%</span>
                </div>
                <div className={styles.trophyBar} style={{ marginTop: 10 }}>
                  <div className={styles.trophyBarFill} style={{ width: `${trophyPercent}%` }} />
                </div>
                <div className={styles.medalRow} style={{ marginTop: 14 }}>
                  {(
                    [
                      ["platinum", "var(--medal-platinum)"],
                      ["gold", "var(--medal-gold)"],
                      ["silver", "var(--medal-silver)"],
                      ["bronze", "var(--medal-bronze)"],
                    ] as [keyof TrophyCounts, string][]
                  ).map(([grade, color]) => (
                    <div key={grade} className={styles.medalCol} title={t(`trophies.${grade}`)}>
                      <Trophy size={14} style={{ color }} />
                      {/* Earned-per-grade isn't tracked separately (only a
                         single earned total exists, see store/trophies.ts) --
                         but since that total is 0 today, each grade's earned
                         is provably 0 too (non-negative counts summing to 0).
                         This numerator stops being "0" the day a per-grade
                         earned breakdown ships alongside real unlock
                         tracking. */}
                      <span className={styles.medalValue}>0/{trophyCounts[grade]}</span>
                    </div>
                  ))}
                </div>
              </div>
              <div>
                <div className={styles.cardFooterLabel}>{t("profileView.trophiesEarned", { value: trophyEarned })}</div>
              </div>
            </div>

            <div className={styles.card}>
              <div className={styles.mostPlayedList}>
                {topPlayed.map((g) => (
                  <MostPlayedRow key={g.config.gamePath} game={g} seconds={history[g.config.gamePath]?.totalSeconds ?? 0} />
                ))}
              </div>
              <div className={styles.cardFooterLabel}>{t("profileView.mostPlayed")}</div>
            </div>

            <div className={styles.card}>
              <div className={styles.gamesCovers}>
                {games.slice(0, 3).map((g) => (
                  <GameCoverThumb key={g.config.gamePath} game={g} className={styles.gamesCover} />
                ))}
              </div>
              <div>
                <div className={styles.cardFooterLabel}>{t("profileView.gamesCount", { value: games.length })}</div>
              </div>
            </div>
          </div>
        </div>

        {recent.length > 0 && (
          <div style={{ padding: "0 56px 48px" }}>
            <div style={{ fontSize: 11, fontWeight: 700, letterSpacing: 1.4, textTransform: "uppercase", color: "var(--text-muted)", marginBottom: 16 }}>
              {t("home.recentlyPlayed")}
            </div>
            <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fill, minmax(150px, 1fr))", gap: 16 }}>
              {recent.map((g) => (
                <RecentEntry key={g.config.gamePath} game={g} seconds={history[g.config.gamePath]?.totalSeconds ?? 0} />
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
  );
}

function RecentEntry({ game, seconds }: { game: GameEntry; seconds: number }) {
  const t = useT();
  const art = useGameArt(game);
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
      <div style={{ aspectRatio: "1 / 1", borderRadius: "var(--radius-lg)", overflow: "hidden", border: "1.5px solid var(--glass-border)", background: "var(--bg-elevated)" }}>
        {art && <img src={art} alt="" style={{ display: "block", width: "100%", height: "100%", objectFit: "cover" }} />}
      </div>
      <div style={{ fontSize: 12, fontWeight: 600, textAlign: "center", color: "var(--text-primary)" }}>{game.config.name}</div>
      <div style={{ fontSize: 10.5, textAlign: "center", color: "var(--text-muted)" }}>{t("home.hoursPlayed")}: {formatPlaytime(seconds)}</div>
    </div>
  );
}

/** One row of the Most-played card: cover, name, total hours. */
function MostPlayedRow({ game, seconds }: { game: GameEntry; seconds: number }) {
  const art = useGameArt(game);
  return (
    <div className={styles.mostPlayedRow}>
      <div className={styles.mostPlayedCover}>{art && <img src={art} alt="" />}</div>
      <span className={styles.mostPlayedName}>{game.config.name}</span>
      <span className={styles.mostPlayedHours}>{formatPlaytime(seconds)}</span>
    </div>
  );
}

/** One cover in the Games card's small preview row. */
function GameCoverThumb({ game, className }: { game: GameEntry; className: string }) {
  const art = useGameArt(game);
  return <div className={className}>{art && <img src={art} alt="" />}</div>;
}
