import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { openPath, openUrl } from "@tauri-apps/plugin-opener";
import { Flag, FolderOpen, Play, Save as SaveIcon, Square, SlidersHorizontal, Trophy, Wrench } from "lucide-react";
import type { Configuration, CompatibilityMap, EmulatorInfo, GameEntry, GameStatus, KytyConfig, PatchStatus } from "../types";
import { buildStatusReportUrl } from "../lib/statusReport";
import { useStore } from "../store/observable";
import { isRunningStore, runningGameStore, runGame, stopGame } from "../store/run";
import { STATUS_CLASS, useGameArt } from "./GameCard";
import { ConfigForm, Field } from "./ConfigForm";
import { configStore, saveConfigAndRescan } from "../store/library";
import { Dropdown } from "./Dropdown";
import { Modal } from "./Modal";
import { Toggle } from "./Toggle";
import { TrophiesView } from "../views/Trophies";
import { useT } from "../i18n";
import formStyles from "../styles/settingsForm.module.css";
import styles from "./GameDetail.module.css";

/** Keyed by GameStatus so the compatibility-status Dropdown's option order
 * matches the enum -- resolved through i18n at use, not stored translated. */
const STATUS_KEYS: Record<GameStatus, string> = {
  Unknown: "gameDetail.status.Unknown",
  InGame: "gameDetail.status.InGame",
  MainMenu: "gameDetail.status.MainMenu",
  Logo: "gameDetail.status.Logo",
  DoesntBoot: "gameDetail.status.DoesntBoot",
};

type Tab = "settings" | "trophies" | "patches" | "saveData";

export function GameDetail({
  game,
  compatibility,
  compatibilityIsLocal,
  onRescanCompatibility,
}: {
  game: GameEntry;
  compatibility: CompatibilityMap;
  compatibilityIsLocal: boolean;
  onRescanCompatibility: () => void;
}) {
  const backdrop = useGameArt(game);
  const t = useT();
  const running = useStore(isRunningStore);
  const runningPath = useStore(runningGameStore);
  const isThisRunning = running && runningPath === game.config.gamePath;

  // Rail + detail, like Settings.tsx's own category system -- always one
  // section active, defaulting to Settings, rather than the old idle
  // hero-card state with no tab open.
  const [activeTab, setActiveTab] = useState<Tab>("settings");
  const [hasTrophies, setHasTrophies] = useState(false);
  const [isPatchable, setIsPatchable] = useState(false);
  const [saveDirs, setSaveDirs] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);
  /** Gates the jump out to the browser behind a confirm step -- leaving the
   * app for an external site is not something a button press should do
   * without saying so first. Modal rather than a native confirm(): the
   * OS dialog's buttons live outside FocusNav's DOM, so a controller-only
   * session could not answer it (the same reasoning the saved-data confirm
   * below is written out for). */
  const [reportPrompt, setReportPrompt] = useState(false);

  const entry = compatibility[game.config.titleId.toUpperCase()];
  const status: GameStatus = entry?.status ?? "Unknown";

  /** What the badge is actually claiming, in small print under it.
   *
   * A bare status is not enough to act on: the community feed reports a
   * cross-platform aggregate alongside a per-OS breakdown, and the two
   * disagree often enough to matter -- a title that is InGame on Linux can
   * be DoesntBoot on Windows. compatibility.rs resolves this platform's own
   * figure when the feed has one, and this says which of the two is on
   * screen, how many reports stand behind it, and which emulator build they
   * were filed against. One report on a build from months ago is a very
   * different claim from twelve on the current one.
   *
   * Nothing here for a locally-edited database: that is the user's own
   * opinion, with no report count or platform split to describe. */
  const compatDetail = (() => {
    if (compatibilityIsLocal || !entry || status === "Unknown") return null;
    const lines: string[] = [
      entry.platformSpecific ? t("gameDetail.compatThisPlatform") : t("gameDetail.compatAllPlatforms"),
    ];
    if (entry.reports === 1) lines.push(t("gameDetail.compatReport"));
    else if (entry.reports > 1) lines.push(t("gameDetail.compatReports", { count: String(entry.reports) }));
    if (entry.version) lines.push(t("gameDetail.compatTestedOn", { value: entry.version }));
    // One line each rather than a single "a · b · c" run: the build string
    // alone is long enough to push that past any sensible tooltip width.
    return lines;
  })();

  useEffect(() => {
    setError(null);
    setActiveTab("settings");
    void invoke<boolean>("has_trophy_data", { basedir: game.config.basedir }).then(setHasTrophies);
    void invoke<boolean>("is_patchable", { titleId: game.config.titleId }).then(setIsPatchable);
    void invoke<string[]>("get_save_data_dirs", { titleId: game.config.titleId }).then(setSaveDirs);
  }, [game.config.gamePath]);

  const run = async () => {
    setError(null);
    try {
      await runGame(game.config, game.config.titleId);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  /** Opens the upstream status-report form with what the launcher already
   * knows filled in. The emulator's version is probed here rather than held
   * in state because it is wanted exactly once, at the moment the user asks
   * to report -- and a failure to probe it is not a reason to refuse the
   * report, only to leave that one field blank for them to type. */
  const reportStatus = async () => {
    setReportPrompt(false);
    let emulatorVersion = "";
    try {
      emulatorVersion = (await invoke<EmulatorInfo>("find_emulator")).version;
    } catch {
      // Leave it blank; the form is still worth opening.
    }
    await openUrl(
      buildStatusReportUrl({
        name: game.config.name,
        titleId: game.config.titleId,
        emulatorVersion,
        status,
      }),
    );
  };

  const setStatus = async (next: GameStatus) => {
    await invoke("compatibility_set_status", { titleId: game.config.titleId, status: next });
    onRescanCompatibility();
  };

  /** Only reachable while the database is local. `compatibility_set_comment`
   * has existed since the port but nothing ever called it, so a user editing
   * their own compatibility notes could set a status and never say why.
   * Committed on blur rather than per keystroke: every save rewrites the
   * whole JSON file and triggers a rescan. */
  const commitComment = async (next: string) => {
    if (next === (entry?.comment ?? "")) return;
    await invoke("compatibility_set_comment", { titleId: game.config.titleId, comment: next });
    onRescanCompatibility();
  };

  const sections: { id: Tab; icon: typeof SlidersHorizontal; label: string }[] = [
    { id: "settings", icon: SlidersHorizontal, label: t("gameDetail.settingsButton") },
  ];
  if (hasTrophies) sections.push({ id: "trophies", icon: Trophy, label: t("gameDetail.trophiesButton") });
  if (isPatchable) sections.push({ id: "patches", icon: Wrench, label: t("gameDetail.patchesButton") });
  if (saveDirs.length > 0) sections.push({ id: "saveData", icon: SaveIcon, label: t("gameDetail.saveDataTab") });

  return (
    <div className={styles.page}>
      <div className={styles.scrim} />
      <div className={styles.body}>
        <div className={styles.header}>
          {/* No back button here anymore -- it duplicated TopBar's chevron,
             which used to always navigate Home and so couldn't return to
             the Library grid from here; now that it routes through
             App.tsx's onBack (shell/TopBar.tsx), it's the only one. */}
          <div className={styles.headerIcon}>
            {backdrop ? <img className={styles.headerIconImage} src={backdrop} alt="" /> : game.config.name.slice(0, 1).toUpperCase()}
          </div>
          <div className={styles.headerInfo}>
            <h1 className={styles.title}>{game.config.name}</h1>
            <div className={styles.metaRow}>
              <span>{t("common.serialLine", { value: game.config.titleId || "-" })}</span>
              <span>{t("common.versionLine", { value: game.config.gameVersion || "-" })}</span>
              <span>{t("common.firmwareLine", { value: game.config.firmwareVer || "-" })}</span>
            </div>
          </div>
          <div className={styles.headerActions}>
            {compatibilityIsLocal ? (
              <div style={{ width: 170, display: "flex", flexDirection: "column", gap: 6 }}>
                <Dropdown
                  ariaLabel={t("gameDetail.compatibilityStatus")}
                  value={status}
                  onChange={(v) => void setStatus(v as GameStatus)}
                  options={Object.entries(STATUS_KEYS).map(([value, key]) => ({ value, label: t(key) }))}
                />
                <input
                  aria-label={t("gameDetail.compatibilityNote")}
                  placeholder={t("gameDetail.compatibilityNote")}
                  defaultValue={entry?.comment ?? ""}
                  key={game.config.titleId}
                  onBlur={(e) => void commitComment(e.target.value)}
                  style={{
                    width: "100%",
                    boxSizing: "border-box",
                    padding: "6px 10px",
                    fontSize: 12,
                    color: "inherit",
                    background: "rgba(255, 255, 255, 0.06)",
                    border: "1px solid rgba(255, 255, 255, 0.14)",
                    borderRadius: 8,
                  }}
                />
              </div>
            ) : (
              <span
                className={styles.compatBadge}
                // Only a nav stop when there is something behind it to
                // reveal; an unknown status has no detail worth stopping on.
                {...(compatDetail ? { "data-focusable": true, "data-focus-key": `compat-${game.config.titleId}` } : {})}
              >
                <span className={`status-dot ${STATUS_CLASS[status] ?? "unknown"}`} />
                {t(STATUS_KEYS[status])}
                {compatDetail && (
                  <span className={styles.compatTip} role="tooltip">
                    {compatDetail.map((line) => (
                      <span key={line}>{line}</span>
                    ))}
                  </span>
                )}
              </span>
            )}
            {isThisRunning ? (
              <button className="pill-button primary" onClick={() => void stopGame()}>
                <Square size={14} fill="currentColor" /> {t("gameDetail.stop")}
              </button>
            ) : (
              <button className="pill-button primary" disabled={running} onClick={run}>
                <Play size={16} fill="#000" /> {t("gameDetail.run")}
              </button>
            )}
            <button className="pill-button" disabled={!game.config.basedir} onClick={() => void openPath(game.config.basedir)}>
              <FolderOpen size={15} /> {t("gameDetail.openFolder")}
            </button>
            <button className="pill-button" disabled={!game.config.titleId} onClick={() => setReportPrompt(true)}>
              <Flag size={15} /> {t("gameDetail.reportStatus")}
            </button>
          </div>
        </div>

        {error && <pre className={styles.error}>{error}</pre>}

        <div className={styles.content}>
          <nav className={styles.rail} data-scroll-region>
            {sections.map((s) => (
              <button
                key={s.id}
                type="button"
                data-focusable
                data-focus-key={`gamedetail-rail-${s.id}`}
                className={activeTab === s.id ? styles.railItemActive : styles.railItem}
                onClick={() => setActiveTab(s.id)}
              >
                <s.icon size={21} strokeWidth={1.6} />
                {s.label}
              </button>
            ))}
          </nav>

          <div className={styles.detail} data-scroll-region>
            <div className={activeTab === "trophies" ? styles.detailFill : styles.detailProse}>
              {activeTab === "settings" && <SettingsPane game={game.config} />}
              {activeTab === "trophies" && <TrophiesView gamePath={game.config.gamePath} />}
              {activeTab === "patches" && <PatchesPane titleId={game.config.titleId} />}
              {activeTab === "saveData" && (
                <SaveDataPane
                  gameName={game.config.name}
                  titleId={game.config.titleId}
                  saveDirs={saveDirs}
                  onSaveDirsChange={setSaveDirs}
                  onDone={() => setActiveTab("settings")}
                />
              )}
            </div>
          </div>
        </div>
      </div>

      {reportPrompt && (
        <Modal
          title={t("gameDetail.reportStatus")}
          width={520}
          dividers={false}
          onClose={() => setReportPrompt(false)}
          footer={
            <>
              <button className="pill-button" onClick={() => setReportPrompt(false)}>
                {t("common.cancel")}
              </button>
              <button className="pill-button primary" onClick={() => void reportStatus()}>
                {t("gameDetail.reportStatusConfirm")}
              </button>
            </>
          }
        >
          <p style={{ margin: 0, fontSize: 14, lineHeight: 1.55 }}>{t("gameDetail.reportStatusPrompt")}</p>
        </Modal>
      )}
    </div>
  );
}

function SettingsPane({ game }: { game: Configuration }) {
  // Remounted (fresh draft) each time this pane opens, since it is keyed by
  // GameDetail's activeTab state -- switching away without saving discards
  // the draft, the same effective behavior a Modal's Cancel button gave.
  const [draft, setDraft] = useState<Configuration>({ ...game });
  // Initialises from game.customSettings, not draft.customSettings -- the
  // switch reflects what is actually saved right now, not a field that
  // changes as the (still-unsaved) draft is edited below.
  const [useCustom, setUseCustom] = useState(game.customSettings);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const t = useT();

  const save = async () => {
    const cfg = configStore.get();
    if (!cfg) return;
    setSaving(true);
    try {
      const withoutThisGame = cfg.gameOverrides.filter((g) => g.gamePath !== draft.gamePath);
      const next: KytyConfig = {
        ...cfg,
        gameOverrides: useCustom ? [...withoutThisGame, { ...draft, customSettings: true }] : withoutThisGame,
      };
      await saveConfigAndRescan(next);
      setSaved(true);
      setTimeout(() => setSaved(false), 1600);
    } finally {
      setSaving(false);
    }
  };

  return (
    <div>
      <h3 className={formStyles.subheading} style={{ marginTop: 0 }}>
        {t("gameSettings.title", { name: game.name })}
      </h3>
      <p className={formStyles.blockHint}>{t("gameSettings.overrideExplanation")}</p>

      <div className={formStyles.row}>
        <div>
          <span className={formStyles.rowLabel}>{t("gameSettings.useCustomSettings")}</span>
          <span className={formStyles.rowHint}>{t("gameSettings.useCustomSettingsHint")}</span>
        </div>
        <Toggle ariaLabel={t("gameSettings.useCustomSettings")} checked={useCustom} onChange={setUseCustom} />
      </div>

      {/* Disabled (not omitted) when the switch is off, so the form still
         shows exactly the values that would apply -- merge_scanned_games
         (scanner.rs) has already copied the global emulator settings onto
         `game` when there is no override, so this is never blank. */}
      <fieldset
        disabled={!useCustom}
        style={{ border: "none", padding: 0, margin: 0, pointerEvents: useCustom ? undefined : "none", opacity: useCustom ? 1 : 0.5 }}
      >
        <Field label={t("gameSettings.bootExecutable")}>
          <input type="text" value={draft.elf} onChange={(e) => setDraft({ ...draft, elf: e.target.value })} />
        </Field>
        <div style={{ height: 14 }} />
        <ConfigForm value={draft} onChange={setDraft} />
      </fieldset>

      <div className={formStyles.actionRow}>
        <button className="pill-button primary" disabled={saving} onClick={() => void save()}>
          {t("common.save")}
        </button>
        {saved && <span style={{ fontSize: 12, color: "var(--success)" }}>{t("settings.defaults.saved")}</span>}
      </div>
    </div>
  );
}

function PatchesPane({ titleId }: { titleId: string }) {
  const t = useT();
  const [status, setStatus] = useState<PatchStatus | null>(null);
  const [enabled, setEnabled] = useState<boolean[]>([]);
  const [saveMessage, setSaveMessage] = useState<string | null>(null);

  useEffect(() => {
    void (async () => {
      const s = await invoke<PatchStatus>("get_patches", { titleId });
      setStatus(s);
      setEnabled(s.patches.map((p) => p.enabled));
    })();
  }, [titleId]);

  const apply = async () => {
    await invoke("save_patches", { titleId, enabled });
    setSaveMessage(t("patches.selectionSaved"));
  };

  return (
    <div>
      <h3 className={formStyles.subheading} style={{ marginTop: 0 }}>
        {t("gameDetail.patchesButton")}
      </h3>
      {status?.patches.map((p, i) => (
        // A <label>, not a <div>: clicking (or gamepad-confirming, which
        // calls el.click()) anywhere in the row natively toggles the
        // nested checkbox with no extra handler needed. Previously this
        // was a bare <div> wrapping a raw <input type="checkbox">, and a
        // checkbox is not in FOCUSABLE_SELECTOR -- spatial nav had no way
        // to reach these rows at all, only the Apply button below them.
        <label key={`${p.name}-${i}`} className={formStyles.row} data-focusable data-focus-key={`patch-${titleId}-${i}`}>
          <span className={formStyles.rowLabel}>{p.name}</span>
          <input
            type="checkbox"
            checked={enabled[i] ?? false}
            onChange={(e) => setEnabled((prev) => prev.map((v, idx) => (idx === i ? e.target.checked : v)))}
          />
        </label>
      ))}
      <p className={formStyles.blockHint}>{saveMessage ?? status?.message}</p>
      <div className={formStyles.actionRow}>
        <button className="pill-button primary" disabled={!status?.patches.length} onClick={() => void apply()}>
          {t("patches.applySelection")}
        </button>
      </div>
    </div>
  );
}

function SaveDataPane({
  gameName,
  titleId,
  saveDirs,
  onSaveDirsChange,
  onDone,
}: {
  gameName: string;
  titleId: string;
  saveDirs: string[];
  onSaveDirsChange: (dirs: string[]) => void;
  onDone: () => void;
}) {
  const t = useT();
  // Replaces a native confirm(): its OK/Cancel are not reachable by this
  // app's own gamepad focus navigation at all (they belong to the OS/webview,
  // outside FocusNav's DOM, and outside its geometric hit-testing), so a
  // controller-only session had no way to answer that dialog. This is an
  // ordinary two-step inline confirm instead, fully covered by FocusNav.
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const confirmRemove = async () => {
    setError(null);
    const failed = await invoke<string[]>("remove_save_data", { dirs: saveDirs });
    if (failed.length) {
      setError(t("gameDetail.removeSaveDataFailed", { list: failed.join("\n") }));
    }
    onSaveDirsChange(await invoke<string[]>("get_save_data_dirs", { titleId }));
    setConfirming(false);
    if (!failed.length) onDone();
  };

  return (
    <div>
      <h3 className={formStyles.subheading} style={{ marginTop: 0 }}>
        {t("gameDetail.saveDataTab")}
      </h3>
      {!saveDirs.length ? (
        <p className={formStyles.blockHint}>{t("gameDetail.noSaveData")}</p>
      ) : (
        saveDirs.map((dir) => (
          // No per-row action (Remove below acts on all saveDirs at once),
          // so data-focusable here only makes the row reachable and
          // scrollable-into-view by spatial nav -- same pattern as
          // Trophies' rows, which are informational in the same way.
          <div key={dir} className={formStyles.row} data-focusable data-focus-key={`savedata-${titleId}-${dir}`}>
            <span className={formStyles.rowLabel} style={{ wordBreak: "break-all" }}>
              {dir}
            </span>
          </div>
        ))
      )}
      {error && <p className={formStyles.blockHint} style={{ color: "var(--error)" }}>{error}</p>}
      {saveDirs.length > 0 && (
        <div className={formStyles.actionRow}>
          {confirming ? (
            <>
              <span className={formStyles.rowHint} style={{ margin: 0 }}>
                {t("gameDetail.confirmRemoveSaveData", { name: gameName })}
              </span>
              <button className="pill-button" onClick={() => setConfirming(false)}>
                {t("common.cancel")}
              </button>
              <button className="pill-button primary" onClick={() => void confirmRemove()}>
                {t("gameDetail.confirmRemoveSaveDataAction")}
              </button>
            </>
          ) : (
            <button className="pill-button" onClick={() => setConfirming(true)}>
              {t("gameDetail.removeSaveData")}
            </button>
          )}
        </div>
      )}
    </div>
  );
}
