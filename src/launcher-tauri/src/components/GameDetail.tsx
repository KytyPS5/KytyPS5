import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { openPath } from "@tauri-apps/plugin-opener";
import { ChevronLeft, FolderOpen, Play, Save as SaveIcon, Square, SlidersHorizontal, Trophy, Wrench } from "lucide-react";
import type { Configuration, CompatibilityMap, GameEntry, GameStatus, KytyConfig, PatchStatus } from "../types";
import { useStore } from "../store/observable";
import { isRunningStore, runningGameStore, runGame, stopGame } from "../store/run";
import { STATUS_CLASS, useGameArt } from "./GameCard";
import { ConfigForm, Field } from "./ConfigForm";
import { configStore, saveConfigAndRescan } from "../store/library";
import { Dropdown } from "./Dropdown";
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
  onBack,
}: {
  game: GameEntry;
  compatibility: CompatibilityMap;
  compatibilityIsLocal: boolean;
  onRescanCompatibility: () => void;
  /** Returns to the Library grid -- this is now a full screen (Library.tsx
   * swaps the grid out for it), not a docked side panel, so it needs its own
   * explicit way back for a mouse user (gamepad/keyboard back already
   * returns here via the focus scope Library.tsx pushes around this). */
  onBack: () => void;
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

  const entry = compatibility[game.config.titleId.toUpperCase()];
  const status: GameStatus = entry?.status ?? "Unknown";

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

  const setStatus = async (next: GameStatus) => {
    await invoke("compatibility_set_status", { titleId: game.config.titleId, status: next });
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
          <button type="button" className="icon-button" onClick={onBack} title={t("common.back")} aria-label={t("common.back")}>
            <ChevronLeft size={20} strokeWidth={2} />
          </button>
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
              <div style={{ width: 170 }}>
                <Dropdown
                  ariaLabel={t("gameDetail.compatibilityStatus")}
                  value={status}
                  onChange={(v) => void setStatus(v as GameStatus)}
                  options={Object.entries(STATUS_KEYS).map(([value, key]) => ({ value, label: t(key) }))}
                />
              </div>
            ) : (
              <span style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 12, color: "var(--text-secondary)" }}>
                <span className={`status-dot ${STATUS_CLASS[status] ?? "unknown"}`} />
                {t(STATUS_KEYS[status])}
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
