import { useEffect, useState } from "react";
import { invoke, convertFileSrc } from "@tauri-apps/api/core";
import type { TrophySet } from "../types";
import { useStore } from "../store/observable";
import { gamesStore } from "../store/library";
import { useT } from "../i18n";

export function TrophiesView({ gamePath }: { gamePath: string | null }) {
  const games = useStore(gamesStore);
  const game = games.find((g) => g.config.gamePath === gamePath);
  const t = useT();

  const [sets, setSets] = useState<TrophySet[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [activeTab, setActiveTab] = useState(0);

  useEffect(() => {
    setSets(null);
    setError(null);
    if (!game) return;
    void invoke<TrophySet[]>("get_trophies", { basedir: game.config.basedir })
      .then((s) => {
        setSets(s);
        setActiveTab(0);
      })
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [game?.config.gamePath]);

  if (!game) {
    return (
      <Centered>{t("trophiesView.selectGameHint")}</Centered>
    );
  }
  if (error) return <Centered tone="error">{error}</Centered>;
  if (!sets) return <Centered>{t("trophiesView.loading")}</Centered>;

  const active = sets[activeTab];

  return (
    <div style={{ padding: "20px 24px", overflow: "hidden", display: "flex", flexDirection: "column", height: "100%" }}>
      <h2 style={{ margin: "0 0 4px", fontSize: 18 }}>{game.config.name}</h2>
      <p style={{ margin: "0 0 16px", fontSize: 12, color: "var(--text-muted)" }}>{t("nav.trophies")}</p>

      {sets.length > 1 && (
        <div style={{ display: "flex", gap: 8, marginBottom: 14 }}>
          {sets.map((s, i) => (
            <button
              key={s.tabTitle}
              className="pill-button"
              onClick={() => setActiveTab(i)}
              style={i === activeTab ? { borderColor: "var(--accent)" } : undefined}
            >
              {s.tabTitle}
            </button>
          ))}
        </div>
      )}

      <div style={{ overflowY: "auto", flex: 1, display: "flex", flexDirection: "column", gap: 10 }} data-scroll-region>
        {active?.trophies.map((trophy) => (
          <div
            key={trophy.id}
            className="glass-panel"
            data-focusable
            data-focus-key={`trophy-${game.config.gamePath}-${activeTab}-${trophy.id}`}
            style={{ display: "flex", gap: 14, padding: 14, alignItems: "center", background: "var(--panel)" }}
          >
            <div
              style={{
                width: 56,
                height: 56,
                borderRadius: "var(--radius-md)",
                background: "var(--glass-bg-strong)",
                flex: "none",
                overflow: "hidden",
                display: "flex",
                alignItems: "center",
                justifyContent: "center",
              }}
            >
              {trophy.iconPath ? (
                <img src={convertFileSrc(trophy.iconPath)} alt="" loading="lazy" decoding="async" style={{ width: "100%", height: "100%", objectFit: "cover" }} />
              ) : (
                <span style={{ fontSize: 20, opacity: 0.4 }}>🏆</span>
              )}
            </div>
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                <span style={{ fontWeight: 700, fontSize: 13.5 }}>{trophy.name}</span>
                <GradeBadge grade={trophy.gradeText} />
              </div>
              <p style={{ margin: "3px 0 0", fontSize: 12, color: "var(--text-secondary)" }}>{trophy.detail}</p>
              {trophy.hasReward && trophy.reward && (
                <p style={{ margin: "3px 0 0", fontSize: 11, color: "var(--text-muted)" }}>{t("trophiesView.reward", { value: trophy.reward })}</p>
              )}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

function GradeBadge({ grade }: { grade: string }) {
  if (!grade) return null;
  const colors: Record<string, string> = {
    Platinum: "var(--medal-platinum)",
    Gold: "var(--medal-gold)",
    Silver: "var(--medal-silver)",
    Bronze: "var(--medal-bronze)",
  };
  return (
    <span
      style={{
        fontSize: 9.5,
        fontWeight: 800,
        textTransform: "uppercase",
        letterSpacing: 0.4,
        padding: "2px 8px",
        borderRadius: "var(--radius-pill)",
        color: colors[grade] ?? "var(--text-muted)",
        border: `1px solid ${colors[grade] ?? "var(--glass-border-strong)"}`,
      }}
    >
      {grade}
    </span>
  );
}

function Centered({ children, tone }: { children: React.ReactNode; tone?: "error" }) {
  return (
    <div
      style={{
        height: "100%",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        padding: 40,
        textAlign: "center",
        color: tone === "error" ? "var(--error)" : "var(--text-muted)",
        fontSize: 13,
      }}
    >
      {children}
    </div>
  );
}
