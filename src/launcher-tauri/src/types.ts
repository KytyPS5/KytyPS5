// Mirrors the Rust structs in src-tauri/src/{config,scanner,emulator,
// patches,trophy,compatibility,prefs,browse}.rs. Field names and enum
// spellings are locked by src-tauri/src/config.rs's
// `json_shape_matches_the_frontend_contract` test — keep the two in sync.

/** `R<width>X<height>`, e.g. `"R2560X1440"` — any resolution, not a closed set. */
export type Resolution = string;
export type ShaderOptimizationType = "None" | "Size" | "Performance";
export type LogDirection = "Silent" | "Console" | "File";
export type ProfilerDirection = "None" | "Network";

export interface Configuration {
  name: string;
  basedir: string;
  gamePath: string;
  customSettings: boolean;

  screenResolution: Resolution;
  fullscreenEnabled: boolean;
  vblankFrequency: number;
  consoleLanguage: number;
  vulkanValidationEnabled: boolean;
  shaderValidationEnabled: boolean;
  shaderOptimizationType: ShaderOptimizationType;
  shaderLogDirection: LogDirection;
  shaderLogFolder: string;
  commandBufferDumpEnabled: boolean;
  commandBufferDumpFolder: string;
  printfDirection: LogDirection;
  printfOutputFile: string;
  profilerDirection: ProfilerDirection;
  renderdocEnabled: boolean;
  bvhStubEnabled: boolean;
  hostInputMapping: string[];
  elf: string;

  titleId: string;
  gameVersion: string;
  firmwareVer: string;
}

export interface KytyConfig {
  settingsFile: string;
  gameDirs: string[];
  global: Configuration;
  gameOverrides: Configuration[];
  /** Physical-gamepad remap ("Control=SdlButtonOrAxisName") and stick
   * deadzone (0.0-0.95). Kyty-Launcher-only, stored outside the sections
   * the Qt launcher rewrites — see config.rs for why. */
  gamepadKeymap: string[];
  gamepadDeadzone: number;
}

export interface GameEntry {
  config: Configuration;
  iconPath: string | null;
  backdropPath: string | null;
  /** Unix ms of the first scan that ever saw this game's path, 0 if
   * unknown. Backs the library's "date added" sort, see
   * src-tauri/src/first_seen.rs. */
  firstSeenMs: number;
}

export interface EmulatorInfo {
  path: string;
  version: string;
}

// Must match Rust's serde(rename_all = "lowercase") on prefs.rs's LaunchMode
// enum exactly -- "InApp" lowercases to "inapp" (the whole variant name, not
// camelCase), not "inApp". Mismatch here previously crashed save_prefs with
// `unknown variant 'inApp', expected 'inapp' or 'terminal'` the moment a user
// actually picked the option (the default never round-tripped, so it went
// unnoticed until this was exercised for real).
export type LaunchMode = "inapp" | "terminal";

export interface LauncherPrefs {
  emulatorPathOverride: string | null;
  launchMode: LaunchMode;
  autoCloseOnLaunch: boolean;
}

export interface BrowseEntry {
  name: string;
  path: string;
  isDir: boolean;
  looksLikeGame: boolean;
}

export interface BrowseResult {
  path: string;
  parent: string | null;
  home: string;
  entries: BrowseEntry[];
}

export interface PatchEntry {
  name: string;
  enabled: boolean;
}

export interface PatchStatus {
  patches: PatchEntry[];
  message: string;
}

export interface TrophyRow {
  id: string;
  name: string;
  detail: string;
  grade: string;
  gradeText: string;
  reward: string;
  hidden: boolean;
  hasReward: boolean;
  iconPath: string | null;
}

export interface TrophySet {
  tabTitle: string;
  trophies: TrophyRow[];
}

export type GameStatus = "Unknown" | "InGame" | "Logo" | "DoesntBoot" | "MainMenu";

export interface CompatibilityEntry {
  status: GameStatus;
  comment: string;
}

export type CompatibilityMap = Record<string, CompatibilityEntry>;

export interface PlayStats {
  lastPlayedMs: number;
  playCount: number;
  totalSeconds: number;
}

export type PlayHistory = Record<string, PlayStats>;

export interface EmulatorLogLine {
  stream: "stdout" | "stderr";
  line: string;
}

export interface EmulatorExitInfo {
  code: number | null;
}
