import { useEffect, useState, type CSSProperties } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { Image, User, Gamepad2, Volume2, Bluetooth, FolderOpen, SlidersHorizontal, Power, Pencil, Trash2, Check, Plus, X } from "lucide-react";
import { useStore } from "../store/observable";
import { configStore, prefsStore, saveConfigAndRescan, savePrefs } from "../store/library";
import { ConfigForm } from "../components/ConfigForm";
import { FolderBrowserModal } from "../components/FolderBrowserModal";
import { ImageBrowserModal } from "../components/ImageBrowserModal";
import { Dropdown } from "../components/Dropdown";
import { Toggle } from "../components/Toggle";
import { ControllerRemapEditor } from "../components/InputMappingDialog";
import { useAudioSettings, setSfxEnabled, setSfxVolume, setOutputSink, listAudioSinks, type AudioSink } from "../lib/audioSettings";
import { playSfx } from "../lib/sfx";
import { LayoutPicker } from "../components/LayoutPicker";
import { cssUrl } from "../lib/cssUrl";
import {
  THEMES,
  CUSTOM_THEME_ID,
  useDashboardTheme,
  setDashboardTheme,
  useCustomBackgroundPath,
  setCustomBackgroundPath,
} from "../lib/theme";
import {
  useActiveProfile,
  useProfiles,
  createProfile,
  updateProfile,
  deleteProfile,
  setActiveProfile,
  readTimeFormat,
  type LauncherProfile,
  type TimeFormat,
} from "../lib/profiles";
import { useDisplayMode, setDisplayMode, type DisplayMode } from "../lib/displayMode";
import { useUiScale, setUiScale, type UiScale } from "../lib/uiScale";
import { LANGUAGES, useT } from "../i18n";
import { consumePendingSettingsCategory } from "../lib/settingsNav";
import type { LaunchMode } from "../types";
import styles from "./Settings.module.css";

type Category = "profile" | "background" | "gamepad" | "audio" | "bluetooth" | "folders" | "defaults" | "system";

const CATEGORY_IDS: Category[] = ["profile", "background", "gamepad", "audio", "bluetooth", "folders", "defaults", "system"];

export function SettingsView() {
  const t = useT();
  const [category, setCategory] = useState<Category>(() => {
    const pending = consumePendingSettingsCategory();
    return pending && (CATEGORY_IDS as string[]).includes(pending) ? (pending as Category) : "background";
  });

  const categories: { id: Category; icon: typeof User; label: string }[] = [
    { id: "profile", icon: User, label: t("nav.profile") },
    { id: "background", icon: Image, label: t("settings.background.title") },
    { id: "gamepad", icon: Gamepad2, label: t("common.gamepad") },
    { id: "audio", icon: Volume2, label: t("settings.audio.title") },
    { id: "bluetooth", icon: Bluetooth, label: t("settings.bluetooth.title") },
    { id: "folders", icon: FolderOpen, label: t("settings.folders.title") },
    { id: "defaults", icon: SlidersHorizontal, label: t("settings.categories.defaults") },
    { id: "system", icon: Power, label: t("settings.system.title") },
  ];
  const activeCategory = categories.find((c) => c.id === category)!;

  return (
    <div className={styles.page}>
      <div className={styles.scrim} />
      <div className={styles.body}>
        <nav className={styles.rail} data-scroll-region>
          {categories.map((c) => (
            <button
              key={c.id}
              type="button"
              data-focusable
              data-focus-key={`settings-rail-${c.id}`}
              className={category === c.id ? styles.railItemActive : styles.railItem}
              onClick={() => setCategory(c.id)}
            >
              <c.icon size={21} strokeWidth={1.6} />
              {c.label}
            </button>
          ))}
        </nav>

        <div className={styles.detailColumn}>
          {/* One shared, bare-text title for every category (owner rule,
             2026-09-09: "unify title styles" -- no icon next to a page
             title, per the real PS5 Settings reference) -- replaces each
             category rendering its own <h1>. Stays fixed above .detail's
             own scroll, matching Library's header staying above its
             scrolling grid. */}
          <h1 className={styles.heading}>{activeCategory.label}</h1>

          <div className={styles.detail} data-scroll-region>
            {category === "gamepad" ? (
              // Not .detailProse (720px is too narrow for the button-mapping
              // grid + controller diagram) -- GamepadCategory measures itself
              // with .gamepadPane instead, sized to the chip+diagram+chip row
              // (see .detail's own doc comment in Settings.module.css).
              <GamepadCategory />
            ) : (
              <div className={styles.detailProse}>
                {category === "profile" && <ProfileCategory />}
                {category === "background" && <BackgroundCategory />}
                {category === "audio" && <AudioCategory />}
                {category === "bluetooth" && <BluetoothCategory />}
                {category === "folders" && <FoldersCategory />}
                {category === "defaults" && <DefaultsCategory />}
                {category === "system" && <SystemCategory />}
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

function BackgroundCategory() {
  const active = useDashboardTheme();
  const customPath = useCustomBackgroundPath();
  const [hoveringCustom, setHoveringCustom] = useState(false);
  const [showImagePicker, setShowImagePicker] = useState(false);
  const displayMode = useDisplayMode();
  const uiScale = useUiScale();
  const t = useT();
  const customLabel = t("settings.background.themes.custom");
  const isCustomActive = active.id === CUSTOM_THEME_ID;
  return (
    <>
      <p className={styles.blockHint}>{t("settings.background.description")}</p>
      {/* All thumbnails (THEMES + the custom slot) fit one row at their
         natural width within .detail's max-width (Settings.module.css) --
         flex-wrap only drops to a second row if the window is narrow enough
         to make that actually necessary, so this stays reactive without a
         fixed column count. */}
      <div style={{ display: "flex", gap: 12, flexWrap: "wrap", marginTop: 20 }}>
        {THEMES.map((theme) => {
          const isActive = theme.id === active.id;
          const label = t(`settings.background.themes.${theme.id}`);
          return (
            // A plain div, not a button -- FocusNav.tsx's FOCUSABLE_SELECTOR
            // treats every <button> as a focus target regardless of
            // data-focusable, so a button here would put the ring back on
            // the whole card (thumbnail + caption) instead of just the
            // thumbnail below.
            <div key={theme.id} className={styles.themeCard} onClick={() => setDashboardTheme(theme.id)} title={label}>
              <div
                data-focusable
                className={`${styles.themeThumb} ${isActive ? styles.themeThumbActive : ""}`}
                style={{ backgroundImage: `url(${theme.url})` }}
              />
              <span className={`${styles.themeLabel} ${isActive ? styles.themeLabelActive : ""}`}>{label}</span>
            </div>
          );
        })}

        {/* Custom slot: empty shows a "+" (click opens the image picker);
           once an image is set, clicking selects it like any other theme,
           and hovering swaps in an "x" to remove it (owner spec,
           2026-09-09) -- stopPropagation so the "x" never also selects. */}
        <div className={styles.themeCard}>
          <div
            data-focusable
            onMouseEnter={() => setHoveringCustom(true)}
            onMouseLeave={() => setHoveringCustom(false)}
            onClick={() => (customPath ? setDashboardTheme(CUSTOM_THEME_ID) : setShowImagePicker(true))}
            title={customLabel}
            className={`${styles.customThumb} ${isCustomActive ? styles.customThumbActive : ""}`}
            style={{
              // backgroundColor, not the `background` shorthand -- the
              // module already sets background-size/position, and the
              // shorthand would reset those back to their initial values.
              backgroundColor: customPath ? undefined : "var(--glass-bg)",
              backgroundImage: customPath ? cssUrl(convertFileSrc(customPath)) : undefined,
            }}
          >
            {!customPath && <Plus size={22} style={{ color: "var(--text-muted)" }} />}
            {customPath && hoveringCustom && (
              <button
                type="button"
                aria-label={t("common.clear")}
                onClick={(e) => {
                  e.stopPropagation();
                  setCustomBackgroundPath(null);
                }}
                className={styles.customClear}
              >
                <X size={13} style={{ color: "#fff" }} />
              </button>
            )}
          </div>
          <span className={`${styles.themeLabel} ${isCustomActive ? styles.themeLabelActive : ""}`}>{customLabel}</span>
        </div>
      </div>

      {showImagePicker && (
        <ImageBrowserModal
          onClose={() => setShowImagePicker(false)}
          onPick={(path) => {
            setCustomBackgroundPath(path);
            setDashboardTheme(CUSTOM_THEME_ID);
            setShowImagePicker(false);
          }}
        />
      )}

      <div className={styles.subsection}>
        {/* marginBottom 20, not the .subheading default of 4 -- matches the
           gap the theme thumbnails above get from .blockHint's 4px bottom
           margin plus their own 20px marginTop (see the thumbnail row
           above). A subheading immediately followed by its picker needs
           the same breathing room as a description followed by its swatches. */}
        <h3 className={styles.subheading} style={{ marginTop: 0, marginBottom: 20 }}>
          {t("settings.background.layout.title")}
        </h3>
        <LayoutPicker />
      </div>

      <div className={styles.rowStack}>
        <span className={styles.subheading} style={{ margin: 0 }}>
          {t("settings.background.displayMode.title")}
        </span>
        <div style={{ width: 220 }}>
          <Dropdown
            ariaLabel={t("settings.background.displayMode.title")}
            value={displayMode}
            onChange={(v) => setDisplayMode(v as DisplayMode)}
            options={[
              { value: "full", label: t("settings.background.displayMode.full") },
              { value: "window", label: t("settings.background.displayMode.window") },
            ]}
          />
        </div>
      </div>

      <div className={styles.rowStack}>
        <span className={styles.subheading} style={{ margin: 0 }}>
          {t("settings.background.textSize.title")}
        </span>
        <div style={{ width: 220 }}>
          <Dropdown
            ariaLabel={t("settings.background.textSize.title")}
            value={uiScale}
            onChange={(v) => setUiScale(v as UiScale)}
            options={[
              { value: "90", label: t("settings.background.textSize.small") },
              { value: "100", label: t("settings.background.textSize.standard") },
              { value: "115", label: t("settings.background.textSize.large") },
              { value: "130", label: t("settings.background.textSize.extraLarge") },
            ]}
          />
        </div>
      </div>
    </>
  );
}

function GamepadCategory() {
  const cfg = useStore(configStore);
  const activeProfile = useActiveProfile();
  const t = useT();
  if (!cfg) return null;

  const saveDeadzone = async (deadzone: number) => {
    await saveConfigAndRescan({ ...cfg, gamepadDeadzone: deadzone });
    if (activeProfile) await updateProfile(activeProfile.id, { gamepadDeadzone: deadzone });
  };

  return (
    <div className={styles.gamepadPane}>
      <p className={styles.blockHint}>{t("settings.gamepad.description")}</p>
      <div className={styles.rowStack}>
        <div style={{ display: "flex", alignItems: "baseline", justifyContent: "space-between", maxWidth: 320 }}>
          <span className={styles.rowLabel}>{t("settings.gamepad.deadzoneLabel")}</span>
          <span style={{ fontSize: 13, color: "var(--text-secondary)", fontVariantNumeric: "tabular-nums" }}>
            {Math.round(cfg.gamepadDeadzone * 100)}%
          </span>
        </div>
        <input
          type="range"
          min={0}
          max={0.5}
          step={0.01}
          value={cfg.gamepadDeadzone}
          style={{ maxWidth: 320, "--range-fill": `${(cfg.gamepadDeadzone / 0.5) * 100}%` } as CSSProperties}
          onChange={(e) => configStore.set({ ...cfg, gamepadDeadzone: Number(e.target.value) })}
          onMouseUp={() => void saveDeadzone(configStore.get()!.gamepadDeadzone)}
          onTouchEnd={() => void saveDeadzone(configStore.get()!.gamepadDeadzone)}
        />
      </div>

      {/* The remap editor itself, live in the page -- no "Configure gamepad
         buttons…" detour. Same controller diagram either mode; every
         capture/clear applies immediately to the live config and, if one is
         active, to the active profile. */}
      <ControllerRemapEditor
        mapping={cfg.global.hostInputMapping}
        gamepadMapping={cfg.gamepadKeymap}
        onChange={async ({ hostInputMapping, gamepadKeymap }) => {
          const c = configStore.get();
          if (!c) return;
          await saveConfigAndRescan({ ...c, global: { ...c.global, hostInputMapping }, gamepadKeymap });
          if (activeProfile) await updateProfile(activeProfile.id, { hostInputMapping, gamepadKeymap });
        }}
      />
    </div>
  );
}

function AudioCategory() {
  const settings = useAudioSettings();
  const [sinks, setSinks] = useState<AudioSink[]>([]);
  const t = useT();

  useEffect(() => {
    void listAudioSinks().then(setSinks);
  }, []);

  const outputOptions = [
    { value: "", label: t("settings.audio.systemDefault") },
    ...sinks.map((s) => ({ value: s.name, label: s.description })),
  ];

  return (
    <>
      <p className={styles.blockHint}>{t("settings.audio.description")}</p>

      <div className={styles.row}>
        <span className={styles.rowLabel}>{t("settings.audio.sfxEnabled")}</span>
        <Toggle
          ariaLabel={t("settings.audio.sfxEnabled")}
          checked={settings.sfxEnabled}
          onChange={(v) => {
            setSfxEnabled(v);
            if (v) playSfx("confirm");
          }}
        />
      </div>

      <div className={styles.rowStack}>
        <span className={styles.rowLabel}>{t("settings.audio.sfxVolume", { percent: Math.round(settings.sfxVolume * 100) })}</span>
        <input
          type="range"
          min={0}
          max={1}
          step={0.05}
          disabled={!settings.sfxEnabled}
          value={settings.sfxVolume}
          style={{ "--range-fill": `${settings.sfxVolume * 100}%` } as CSSProperties}
          onChange={(e) => setSfxVolume(Number(e.target.value))}
          onMouseUp={() => playSfx("nav")}
          onTouchEnd={() => playSfx("nav")}
        />
      </div>

      <div className={styles.row}>
        <span className={styles.rowLabel}>{t("settings.audio.outputDevice")}</span>
        <div style={{ width: 260 }}>
          <Dropdown
            ariaLabel={t("settings.audio.outputDevice")}
            value={settings.outputSink ?? ""}
            onChange={(v) => void setOutputSink(v || null)}
            options={outputOptions}
          />
        </div>
      </div>
      {sinks.length === 0 && <p className={styles.blockHint}>{t("settings.audio.noDevicesFound")}</p>}
    </>
  );
}

interface BtDevice {
  address: string;
  name: string;
  paired: boolean;
  connected: boolean;
}

/** Pairing has no capture-overlay equivalent -- there is no button chord to
 * catch, so this is a plain list + async action buttons, the same shape as
 * FoldersCategory just above. Loading `list_bluetooth_devices` on mount
 * shows whatever BlueZ already knows (paired devices survive a restart);
 * "Scan" is the only action that can ever add a new, not-yet-paired one to
 * the list, since BlueZ only tracks a device once discovery has seen it. */
function BluetoothCategory() {
  const t = useT();
  const [devices, setDevices] = useState<BtDevice[]>([]);
  const [scanning, setScanning] = useState(false);
  const [busyAddress, setBusyAddress] = useState<string | null>(null);
  const [loadError, setLoadError] = useState(false);

  const refresh = async () => {
    try {
      setDevices(await invoke<BtDevice[]>("list_bluetooth_devices"));
      setLoadError(false);
    } catch {
      setLoadError(true);
    }
  };

  useEffect(() => {
    void refresh();
  }, []);

  const scan = async () => {
    setScanning(true);
    try {
      setDevices(await invoke<BtDevice[]>("scan_bluetooth_devices"));
      setLoadError(false);
    } catch {
      setLoadError(true);
    } finally {
      setScanning(false);
    }
  };

  const withBusy = async (address: string, action: () => Promise<void>) => {
    setBusyAddress(address);
    try {
      await action();
    } catch {
      // Best-effort -- BlueZ already surfaces the real reason (rejected
      // pairing, device out of range) via its own system notification;
      // this list just reflects whatever state resulted.
    } finally {
      setBusyAddress(null);
      await refresh();
    }
  };

  const paired = devices.filter((d) => d.paired);
  const available = devices.filter((d) => !d.paired);

  const renderDevice = (d: BtDevice) => (
    <div key={d.address} className={styles.row}>
      <div style={{ display: "flex", flexDirection: "column", gap: 2, minWidth: 0 }}>
        <span style={{ fontSize: 14, color: "var(--text-primary)" }}>{d.name}</span>
        {d.paired && (
          <span style={{ fontSize: 11.5, color: d.connected ? "var(--success)" : "var(--text-muted)" }}>{d.address}</span>
        )}
      </div>
      <div style={{ display: "flex", gap: 8, flex: "none" }}>
        {d.paired && d.connected && (
          <button
            type="button"
            className="pill-button"
            disabled={busyAddress === d.address}
            onClick={() => void withBusy(d.address, () => invoke("disconnect_bluetooth_device", { address: d.address }))}
          >
            {t("settings.bluetooth.disconnect")}
          </button>
        )}
        {d.paired && !d.connected && (
          <button
            type="button"
            className="pill-button primary"
            disabled={busyAddress === d.address}
            onClick={() => void withBusy(d.address, () => invoke("connect_bluetooth_device", { address: d.address }))}
          >
            {t("settings.bluetooth.connect")}
          </button>
        )}
        {!d.paired && (
          <button
            type="button"
            className="pill-button primary"
            disabled={busyAddress === d.address}
            onClick={() => void withBusy(d.address, () => invoke("pair_bluetooth_device", { address: d.address }))}
          >
            {t("settings.bluetooth.connect")}
          </button>
        )}
        {d.paired && (
          <button
            type="button"
            className="pill-button"
            disabled={busyAddress === d.address}
            onClick={() => void withBusy(d.address, () => invoke("forget_bluetooth_device", { address: d.address }))}
          >
            {t("settings.bluetooth.forget")}
          </button>
        )}
      </div>
    </div>
  );

  return (
    <>
      <p className={styles.blockHint}>{t("settings.bluetooth.description")}</p>

      <div className={styles.actionRow}>
        <button type="button" className="pill-button" disabled={scanning} onClick={() => void scan()}>
          {scanning ? t("settings.bluetooth.scanning") : t("settings.bluetooth.scan")}
        </button>
      </div>

      {loadError && <p className={styles.blockHint}>{t("settings.bluetooth.noDevices")}</p>}

      {paired.length > 0 && (
        <>
          <h3 className={styles.subheading}>{t("settings.bluetooth.paired")}</h3>
          {paired.map(renderDevice)}
        </>
      )}

      <h3 className={styles.subheading}>{t("settings.bluetooth.available")}</h3>
      {available.length === 0 ? (
        <p className={styles.blockHint}>{t("settings.bluetooth.noDevices")}</p>
      ) : (
        available.map(renderDevice)
      )}
    </>
  );
}

function FoldersCategory() {
  const cfg = useStore(configStore);
  const t = useT();
  const [showFolderPicker, setShowFolderPicker] = useState(false);
  if (!cfg) return null;

  const addFolder = async (path: string) => {
    setShowFolderPicker(false);
    if (cfg.gameDirs.includes(path)) return;
    await saveConfigAndRescan({ ...cfg, gameDirs: [...cfg.gameDirs, path] });
  };

  return (
    <>
      <p className={styles.blockHint}>{t("settings.folders.description")}</p>
      {cfg.gameDirs.length === 0 && <p className={styles.blockHint}>{t("settings.folders.none")}</p>}
      {cfg.gameDirs.map((dir) => (
        <div key={dir} className={styles.row}>
          <span style={{ fontSize: 13, color: "var(--text-primary)", wordBreak: "break-all" }}>{dir}</span>
          <button
            className="pill-button"
            onClick={() => void saveConfigAndRescan({ ...cfg, gameDirs: cfg.gameDirs.filter((d) => d !== dir) })}
          >
            {t("settings.folders.remove")}
          </button>
        </div>
      ))}
      <div className={styles.actionRow}>
        <button className="pill-button primary" onClick={() => setShowFolderPicker(true)}>
          {t("settings.folders.add")}
        </button>
      </div>
      {showFolderPicker && <FolderBrowserModal onPick={addFolder} onClose={() => setShowFolderPicker(false)} />}
    </>
  );
}

function DefaultsCategory() {
  const cfg = useStore(configStore);
  const prefs = useStore(prefsStore);
  const t = useT();
  const [saved, setSaved] = useState(false);
  if (!cfg) return null;

  const saveGlobal = async () => {
    await saveConfigAndRescan(cfg);
    setSaved(true);
    setTimeout(() => setSaved(false), 1600);
  };

  return (
    <>
      <p className={styles.blockHint}>{t("settings.defaults.title")}</p>

      {/* Launch mode used to be its own top-level category (settingsNav
         only ever deep-links "profile", so folding it in here breaks no
         external reference) -- it's one emulator-launch setting, same
         family as everything else on this page. */}
      {prefs && (
        <div className={styles.row}>
          <span className={styles.rowLabel}>{t("settings.launch.mode")}</span>
          <div style={{ width: 260 }}>
            <Dropdown
              ariaLabel={t("settings.launch.mode")}
              value={prefs.launchMode}
              onChange={(v) => void savePrefs({ ...prefs, launchMode: v as LaunchMode })}
              options={[
                { value: "inapp", label: t("settings.launch.modeInApp") },
                { value: "terminal", label: t("settings.launch.modeTerminal") },
              ]}
            />
          </div>
        </div>
      )}

      {prefs && (
        <div className={styles.autoCloseRow}>
          <span className={styles.rowLabel}>{t("settings.launch.autoClose")}</span>
          <Toggle
            ariaLabel={t("settings.launch.autoClose")}
            checked={prefs.autoCloseOnLaunch}
            onChange={(v) => void savePrefs({ ...prefs, autoCloseOnLaunch: v })}
          />
        </div>
      )}
      {prefs && <p className={styles.autoCloseHint}>{t("settings.launch.autoCloseHint")}</p>}

      <ConfigForm value={cfg.global} onChange={(next) => configStore.set({ ...cfg, global: next })} />
      <div className={styles.actionRow}>
        <button className="pill-button primary" onClick={saveGlobal}>
          {t("settings.defaults.save")}
        </button>
        {saved && <span style={{ fontSize: 12, color: "var(--success)" }}>{t("settings.defaults.saved")}</span>}
      </div>
    </>
  );
}

function SystemCategory() {
  const t = useT();
  return (
    <>
      <p className={styles.blockHint}>{t("settings.system.description")}</p>
      <div className={styles.row} style={{ borderBottom: "none" }}>
        <button type="button" className="pill-button" onClick={() => void getCurrentWindow().close()}>
          <Power size={15} style={{ marginRight: 4 }} />
          {t("settings.system.exit")}
        </button>
      </div>
    </>
  );
}

function ProfileCategory() {
  const profiles = useProfiles();
  const active = useActiveProfile();
  const t = useT();
  const [newName, setNewName] = useState("");
  const [renamingId, setRenamingId] = useState<string | null>(null);
  const [renameDraft, setRenameDraft] = useState("");

  const submitNew = () => {
    const name = newName.trim();
    if (!name) return;
    const created = createProfile(name);
    setNewName("");
    void setActiveProfile(created.id);
  };

  const submitRename = (id: string) => {
    const name = renameDraft.trim();
    setRenamingId(null);
    if (!name) return;
    void updateProfile(id, { name });
  };

  return (
    <>
      <p className={styles.blockHint}>{t("profile.description")}</p>

      {profiles.map((p: LauncherProfile) => {
        const isActive = p.id === active?.id;
        return (
          <div key={p.id} className={styles.row}>
            <div
              {...(!isActive && { role: "button", tabIndex: 0, "data-focusable": true, "data-focus-key": `profile-row-${p.id}` })}
              className={isActive ? styles.profileRowActive : styles.profileRow}
              onClick={() => !isActive && void setActiveProfile(p.id)}
            >
              <div
                style={{
                  width: 34,
                  height: 34,
                  borderRadius: "50%",
                  flex: "none",
                  display: "flex",
                  alignItems: "center",
                  justifyContent: "center",
                  background: "var(--glass-bg-strong)",
                  color: "var(--text-primary)",
                  fontFamily: "var(--font-display)",
                  fontWeight: 800,
                  fontSize: 14,
                }}
              >
                {p.name.slice(0, 1).toUpperCase()}
              </div>
              {renamingId === p.id ? (
                <input
                  autoFocus
                  type="text"
                  value={renameDraft}
                  onChange={(e) => setRenameDraft(e.target.value)}
                  onKeyDown={(e) => e.key === "Enter" && submitRename(p.id)}
                  onBlur={() => submitRename(p.id)}
                  style={{ maxWidth: 220 }}
                />
              ) : (
                <span style={{ fontSize: 14, color: "var(--text-primary)" }}>{p.name}</span>
              )}
              {isActive && (
                <span style={{ display: "inline-flex", alignItems: "center", gap: 4, fontSize: 11, color: "var(--success)" }}>
                  <Check size={13} /> {t("profile.active")}
                </span>
              )}
            </div>
            <div style={{ display: "flex", gap: 4 }}>
              <button
                type="button"
                className="icon-button"
                title={t("profile.rename")}
                onClick={() => {
                  setRenamingId(p.id);
                  setRenameDraft(p.name);
                }}
              >
                <Pencil size={15} />
              </button>
              <button
                type="button"
                className="icon-button"
                title={profiles.length <= 1 ? t("profile.cannotDeleteLast") : t("profile.delete")}
                disabled={profiles.length <= 1}
                onClick={() => void deleteProfile(p.id)}
              >
                <Trash2 size={15} />
              </button>
            </div>
          </div>
        );
      })}

      <div className={styles.row} style={{ gap: 10 }}>
        <input
          type="text"
          placeholder={t("profile.namePlaceholder")}
          value={newName}
          onChange={(e) => setNewName(e.target.value)}
          onKeyDown={(e) => e.key === "Enter" && submitNew()}
          style={{ flex: 1, maxWidth: 260 }}
        />
        <button className="pill-button" onClick={submitNew}>
          {t("profile.newProfile")}
        </button>
      </div>

      {active && (
        <div className={styles.row}>
          <span className={styles.rowLabel}>{t("profile.language")}</span>
          <div style={{ width: 220 }}>
            <Dropdown
              ariaLabel={t("profile.language")}
              value={active.localeCode}
              onChange={(v) => void updateProfile(active.id, { localeCode: v })}
              options={LANGUAGES.map((l) => ({ value: l.code, label: l.endonym }))}
            />
          </div>
        </div>
      )}

      {active && (
        <div className={styles.row}>
          <span className={styles.rowLabel}>{t("profile.timeFormat")}</span>
          <div style={{ width: 220 }}>
            <Dropdown
              ariaLabel={t("profile.timeFormat")}
              value={readTimeFormat(active)}
              onChange={(v) => void updateProfile(active.id, { timeFormat: v as TimeFormat })}
              options={[
                { value: "24h", label: t("profile.timeFormat24h") },
                { value: "12h", label: t("profile.timeFormat12h") },
              ]}
            />
          </div>
        </div>
      )}

    </>
  );
}
