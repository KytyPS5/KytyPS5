import { useEffect, useRef, useState } from "react";
import { Copy, FolderOpen, Terminal, Trash2 } from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { openPath } from "@tauri-apps/plugin-opener";
import { useStore } from "../store/observable";
import { clearLogs, isRunningStore, lastExitCodeStore, logLinesStore, runningGameStore, stopGame } from "../store/run";
import { gamesStore } from "../store/library";
import { GlassPanel } from "../components/GlassPanel";
import { useT } from "../i18n";
import pageHeaderStyles from "../styles/pageHeader.module.css";

export function LogsView() {
  const lines = useStore(logLinesStore);
  const running = useStore(isRunningStore);
  const runningPath = useStore(runningGameStore);
  const exitCode = useStore(lastExitCodeStore);
  const games = useStore(gamesStore);
  const game = games.find((g) => g.config.gamePath === runningPath);
  const t = useT();

  const [copied, setCopied] = useState(false);
  const copyLogs = async () => {
    const text = lines.map((l) => (l.stream === "stderr" ? `[stderr] ${l.line}` : l.line)).join("\n");
    await navigator.clipboard.writeText(text);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  const openLogsFolder = async () => {
    const dir = await invoke<string>("get_logs_dir");
    await openPath(dir);
  };

  const scrollRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    scrollRef.current?.scrollTo({ top: scrollRef.current.scrollHeight });
  }, [lines.length]);

  return (
    <div style={{ position: "relative", height: "100%", overflow: "hidden" }}>
      <div
        style={{
          position: "absolute",
          inset: 0,
          background: "linear-gradient(115deg, rgba(5, 8, 18, 0.93) 0%, rgba(5, 8, 18, 0.82) 42%, rgba(5, 8, 18, 0.55) 100%)",
        }}
      />

      <div style={{ position: "relative", zIndex: 1, height: "100%", display: "flex", flexDirection: "column", padding: "28px 40px" }}>
        <div style={{ display: "flex", alignItems: "center", gap: 14, marginBottom: 18 }}>
          {/* Shared page-title style (styles/pageHeader.module.css), not this
             file's own inline font values -- owner rule, 2026-09-09: "unify
             title styles"; this file's h1 had drifted from that (26px/700
             instead of the shared 32px/300), the only page title that had.
             Rest of this file stays inline-styled (no Logs.module.css of its
             own), so this imports the shared module directly rather than
             introducing a whole new CSS module for one rule. */}
          <h1 className={pageHeaderStyles.title}>{t("console.title")}</h1>

          {running ? (
            <span
              style={{
                display: "flex",
                alignItems: "center",
                gap: 8,
                fontSize: 12.5,
                fontWeight: 600,
                color: "var(--success)",
                background: "rgba(0, 230, 118, 0.12)",
                border: "1px solid rgba(0, 230, 118, 0.3)",
                borderRadius: "var(--radius-pill)",
                padding: "6px 14px",
              }}
            >
              <span className="status-dot in-game" style={{ animation: "ps-pulse 1.6s ease-in-out infinite" }} />
              {game ? game.config.name : t("common.running")}
            </span>
          ) : (
            <span style={{ fontSize: 12.5, color: "var(--text-muted)" }}>
              {exitCode !== null ? t("console.lastRunExited", { code: exitCode }) : t("console.notRunning")}
            </span>
          )}

          <div style={{ marginLeft: "auto", display: "flex", alignItems: "center", gap: 10 }}>
            {copied && <span style={{ fontSize: 12, color: "var(--success)" }}>{t("console.copiedToClipboard")}</span>}
            <button className="pill-button" onClick={() => void copyLogs()} disabled={lines.length === 0}>
              <Copy size={14} /> {t("console.copyLogs")}
            </button>
            {/* The session files, which outlive this pane -- what a user
                attaches to a bug report rather than retyping an error. */}
            <button className="pill-button" onClick={() => void openLogsFolder()}>
              <FolderOpen size={14} /> {t("console.openLogsFolder")}
            </button>
            <button className="pill-button" onClick={clearLogs} disabled={lines.length === 0}>
              <Trash2 size={14} /> {t("console.clear")}
            </button>
            {running && (
              <button className="pill-button" onClick={() => void stopGame()}>
                ■ {t("console.stopButton")}
              </button>
            )}
          </div>
        </div>

        <GlassPanel variant="heavy" style={{ flex: 1, minHeight: 0, display: "flex", overflow: "hidden" }}>
          <div
            ref={scrollRef}
            data-scroll-region
            data-selectable
            style={{
              flex: 1,
              overflowY: "auto",
              padding: 18,
              fontFamily: "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace",
              fontSize: 12.5,
              lineHeight: 1.7,
            }}
          >
            {lines.length === 0 ? (
              <div style={{ height: "100%", display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 10, color: "var(--text-muted)", textAlign: "center" }}>
                <Terminal size={28} strokeWidth={1.5} style={{ opacity: 0.5 }} />
                <p style={{ margin: 0, fontFamily: "var(--font-body)", fontSize: 13 }}>{t("console.emptyHint")}</p>
              </div>
            ) : (
              lines.map((l, i) =>
                l.stream === "stderr" ? (
                  <div
                    key={i}
                    style={{
                      color: "var(--error)",
                      borderLeft: "2px solid var(--error)",
                      paddingLeft: 10,
                      marginLeft: -12,
                    }}
                  >
                    {l.line}
                  </div>
                ) : (
                  <div key={i} style={{ color: "var(--text-secondary)" }}>
                    {l.line}
                  </div>
                ),
              )
            )}
          </div>
        </GlassPanel>
      </div>
    </div>
  );
}
