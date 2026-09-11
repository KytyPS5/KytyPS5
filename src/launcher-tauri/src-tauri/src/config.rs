//! `Configuration` model and `Kyty.ini` load/save — the Rust mirror of
//! `src/launcher/include/configuration.h` and the settings read/write halves
//! of `src/launcher/src/configurationListWidget.cpp`. Field names, defaults,
//! and enum spellings are kept identical on purpose so the ini file this
//! writes is indistinguishable from one the Qt launcher wrote.

use crate::qsettings::{
    array_key, decode_bool, decode_f64, decode_int, decode_string, decode_string_list,
    encode_bool, encode_f64, encode_int, encode_string, encode_string_list, IniDocument,
};
use serde::{Deserialize, Serialize};
use std::fmt;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

pub const DEFAULT_CONSOLE_LANGUAGE: i64 = 1;
pub const MAX_CONSOLE_LANGUAGE: i64 = 29;

macro_rules! string_enum {
    ($name:ident { $($variant:ident => $text:literal),+ $(,)? }, default = $default:ident) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub enum $name {
            $($variant),+
        }

        impl $name {
            pub fn as_ini_text(&self) -> &'static str {
                match self {
                    $(Self::$variant => $text),+
                }
            }

            pub fn from_ini_text(text: &str) -> Self {
                match text {
                    $($text => Self::$variant,)+
                    _ => Self::$default,
                }
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self::$default
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "{}", self.as_ini_text())
            }
        }

        // Serialize/deserialize through the same ini-text spelling used on
        // disk and by the CLI, rather than deriving on the Rust identifiers
        // — `ShaderOptimizationType::None_`'s trailing underscore (a
        // keyword-collision workaround) must never leak into JSON.
        impl Serialize for $name {
            fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                serializer.serialize_str(self.as_ini_text())
            }
        }

        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
                let text = String::deserialize(deserializer)?;
                Ok(Self::from_ini_text(&text))
            }
        }
    };
}

string_enum!(ShaderOptimizationType { None_ => "None", Size => "Size", Performance => "Performance" }, default = Performance);
string_enum!(LogDirection { Silent => "Silent", Console => "Console", File => "File" }, default = Silent);
string_enum!(ProfilerDirection { None_ => "None", Network => "Network" }, default = None_);

/// Qt's `ScreenResolution` enum only ever had two members (`R1280X720`,
/// `R1920X1080`) — a real limitation of the shared Qt launcher, not just this
/// port. The emulator itself takes plain `--screen-width`/`--screen-height`
/// integers with no such restriction, so this type generalizes the same
/// `R<width>X<height>` ini spelling (confirmed against a real
/// `screen_resolution=R1280X720`) to any resolution: Qt still reads the two
/// values it knows and falls back to its own default on anything else, while
/// this launcher supports whatever the user's display actually offers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Resolution {
    pub width: u32,
    pub height: u32,
}

impl Resolution {
    pub const fn new(width: u32, height: u32) -> Self {
        Self { width, height }
    }

    /// The `<width>x<height>` spelling `--screen-width`/`--screen-height`
    /// are derived from (see `CreateEmulatorArgs` in mainDialog.cpp).
    pub fn dimensions(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn as_ini_text(&self) -> String {
        format!("R{}X{}", self.width, self.height)
    }

    pub fn from_ini_text(text: &str) -> Self {
        text.strip_prefix('R')
            .and_then(|rest| rest.split_once('X'))
            .and_then(|(w, h)| Some(Self::new(w.parse().ok()?, h.parse().ok()?)))
            .unwrap_or_default()
    }
}

impl Default for Resolution {
    fn default() -> Self {
        Self::new(1280, 720)
    }
}

impl fmt::Display for Resolution {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.as_ini_text())
    }
}

impl Serialize for Resolution {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&self.as_ini_text())
    }
}

impl<'de> Deserialize<'de> for Resolution {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let text = String::deserialize(deserializer)?;
        Ok(Self::from_ini_text(&text))
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Configuration {
    pub name: String,
    pub basedir: String,
    pub game_path: String,
    pub custom_settings: bool,

    pub screen_resolution: Resolution,
    pub fullscreen_enabled: bool,
    pub vblank_frequency: i64,
    pub console_language: i64,
    pub vulkan_validation_enabled: bool,
    pub shader_validation_enabled: bool,
    pub shader_optimization_type: ShaderOptimizationType,
    pub shader_log_direction: LogDirection,
    pub shader_log_folder: String,
    pub command_buffer_dump_enabled: bool,
    pub command_buffer_dump_folder: String,
    pub printf_direction: LogDirection,
    pub printf_output_file: String,
    pub profiler_direction: ProfilerDirection,
    pub renderdoc_enabled: bool,
    /// `--stub-bvh`: an inert always-miss stub for the MIMG BVH ray-intersect
    /// opcodes (0xe6/0xe7), which this emulator does not implement. Off by
    /// default -- rendering is wrong wherever a game's ray tracing actually
    /// matters, so this trades a clear "not implemented" failure for a game
    /// that runs with degraded visuals; the user opts in per game or globally
    /// deliberately, same posture as the upstream flag it mirrors.
    pub bvh_stub_enabled: bool,
    pub host_input_mapping: Vec<String>,
    pub elf: String,

    // Not persisted to Kyty.ini — derived fresh from sce_sys/param.json on
    // every scan, exactly like the Qt launcher's Configuration::title_id /
    // gameVersion / firmwareVer.
    #[serde(default)]
    pub title_id: String,
    #[serde(default)]
    pub game_version: String,
    #[serde(default)]
    pub firmware_ver: String,
}

impl Default for Configuration {
    fn default() -> Self {
        Configuration {
            name: String::new(),
            basedir: String::new(),
            game_path: String::new(),
            custom_settings: false,
            screen_resolution: Resolution::default(),
            fullscreen_enabled: false,
            vblank_frequency: 60,
            console_language: DEFAULT_CONSOLE_LANGUAGE,
            vulkan_validation_enabled: false,
            shader_validation_enabled: true,
            shader_optimization_type: ShaderOptimizationType::default(),
            shader_log_direction: LogDirection::default(),
            shader_log_folder: "_Shaders".to_string(),
            command_buffer_dump_enabled: false,
            command_buffer_dump_folder: "_Buffers".to_string(),
            printf_direction: LogDirection::default(),
            printf_output_file: "_kyty.txt".to_string(),
            profiler_direction: ProfilerDirection::default(),
            renderdoc_enabled: false,
            bvh_stub_enabled: false,
            host_input_mapping: Vec::new(),
            elf: "eboot.bin".to_string(),
            title_id: String::new(),
            game_version: String::new(),
            firmware_ver: String::new(),
        }
    }
}

impl Configuration {
    /// Copy only the emulator-behavior fields, mirroring
    /// `Configuration::CopyEmulatorSettingsFrom` — used when a freshly
    /// scanned game has no custom settings yet and should inherit the
    /// global defaults.
    pub fn copy_emulator_settings_from(&mut self, other: &Configuration) {
        self.screen_resolution = other.screen_resolution;
        self.fullscreen_enabled = other.fullscreen_enabled;
        self.vblank_frequency = other.vblank_frequency;
        self.console_language = other.console_language;
        self.vulkan_validation_enabled = other.vulkan_validation_enabled;
        self.shader_validation_enabled = other.shader_validation_enabled;
        self.shader_optimization_type = other.shader_optimization_type;
        self.shader_log_direction = other.shader_log_direction;
        self.shader_log_folder = other.shader_log_folder.clone();
        self.command_buffer_dump_enabled = other.command_buffer_dump_enabled;
        self.command_buffer_dump_folder = other.command_buffer_dump_folder.clone();
        self.printf_direction = other.printf_direction;
        self.printf_output_file = other.printf_output_file.clone();
        self.profiler_direction = other.profiler_direction;
        self.renderdoc_enabled = other.renderdoc_enabled;
        self.bvh_stub_enabled = other.bvh_stub_enabled;
        self.host_input_mapping = other.host_input_mapping.clone();
    }

    fn write_into(&self, doc: &mut IniDocument, section: &str, prefix: &str) {
        let k = |field: &str| format!("{prefix}{field}");
        doc.set(section, &k("name"), encode_string(&self.name));
        doc.set(section, &k("basedir"), encode_string(&self.basedir));
        doc.set(section, &k("game_path"), encode_string(&self.game_path));
        doc.set(section, &k("custom_settings"), encode_bool(self.custom_settings));
        doc.set(section, &k("screen_resolution"), self.screen_resolution.as_ini_text());
        doc.set(section, &k("fullscreen_enabled"), encode_bool(self.fullscreen_enabled));
        doc.set(section, &k("vblank_frequency"), encode_int(self.vblank_frequency));
        doc.set(section, &k("console_language"), encode_int(self.console_language));
        doc.set(section, &k("vulkan_validation_enabled"), encode_bool(self.vulkan_validation_enabled));
        doc.set(section, &k("shader_validation_enabled"), encode_bool(self.shader_validation_enabled));
        doc.set(
            section,
            &k("shader_optimization_type"),
            self.shader_optimization_type.as_ini_text().to_string(),
        );
        doc.set(section, &k("shader_log_direction"), self.shader_log_direction.as_ini_text().to_string());
        doc.set(section, &k("shader_log_folder"), encode_string(&self.shader_log_folder));
        doc.set(section, &k("command_buffer_dump_enabled"), encode_bool(self.command_buffer_dump_enabled));
        doc.set(section, &k("command_buffer_dump_folder"), encode_string(&self.command_buffer_dump_folder));
        doc.set(section, &k("printf_direction"), self.printf_direction.as_ini_text().to_string());
        doc.set(section, &k("printf_output_file"), encode_string(&self.printf_output_file));
        doc.set(section, &k("profiler_direction"), self.profiler_direction.as_ini_text().to_string());
        doc.set(section, &k("renderdoc_enabled"), encode_bool(self.renderdoc_enabled));
        // Written only when switched on. This key is a launcher-tauri
        // addition that the Qt launcher knows nothing about, so emitting it
        // unconditionally would inject a foreign line into every Kyty.ini
        // this launcher touches -- including files the user goes back and
        // forth to the Qt launcher with. Off is the default, so absent and
        // `false` mean the same thing on load; leaving it out keeps a
        // Qt-written file byte-identical through a load/save cycle.
        if self.bvh_stub_enabled {
            doc.set(section, &k("bvh_stub_enabled"), encode_bool(true));
        } else {
            doc.remove(section, &k("bvh_stub_enabled"));
        }
        doc.set(section, &k("host_input_mapping"), encode_string_list(&self.host_input_mapping));
        doc.set(section, &k("elf"), encode_string(&self.elf));
    }

    fn read_from(doc: &IniDocument, section: &str, prefix: &str) -> Self {
        let mut c = Configuration::default();
        let k = |field: &str| format!("{prefix}{field}");
        let get = |field: &str| doc.get(section, &k(field));

        if let Some(v) = get("name") {
            c.name = decode_string(v);
        }
        if let Some(v) = get("basedir") {
            c.basedir = decode_string(v);
        }
        if let Some(v) = get("game_path") {
            c.game_path = decode_string(v);
        }
        if let Some(v) = get("custom_settings") {
            c.custom_settings = decode_bool(v);
        }
        if let Some(v) = get("screen_resolution") {
            c.screen_resolution = Resolution::from_ini_text(v);
        }
        if let Some(v) = get("fullscreen_enabled") {
            c.fullscreen_enabled = decode_bool(v);
        }
        c.vblank_frequency = get("vblank_frequency")
            .map(|v| decode_int(v, c.vblank_frequency))
            .unwrap_or(c.vblank_frequency);
        c.console_language = get("console_language")
            .map(|v| decode_int(v, c.console_language))
            .unwrap_or(c.console_language);
        if !(0..=MAX_CONSOLE_LANGUAGE).contains(&c.console_language) {
            c.console_language = DEFAULT_CONSOLE_LANGUAGE;
        }
        if let Some(v) = get("vulkan_validation_enabled") {
            c.vulkan_validation_enabled = decode_bool(v);
        }
        if let Some(v) = get("shader_validation_enabled") {
            c.shader_validation_enabled = decode_bool(v);
        }
        if let Some(v) = get("shader_optimization_type") {
            c.shader_optimization_type = ShaderOptimizationType::from_ini_text(v);
        }
        if let Some(v) = get("shader_log_direction") {
            c.shader_log_direction = LogDirection::from_ini_text(v);
        }
        if let Some(v) = get("shader_log_folder") {
            c.shader_log_folder = decode_string(v);
        }
        if let Some(v) = get("command_buffer_dump_enabled") {
            c.command_buffer_dump_enabled = decode_bool(v);
        }
        if let Some(v) = get("command_buffer_dump_folder") {
            c.command_buffer_dump_folder = decode_string(v);
        }
        if let Some(v) = get("printf_direction") {
            c.printf_direction = LogDirection::from_ini_text(v);
        }
        if let Some(v) = get("printf_output_file") {
            c.printf_output_file = decode_string(v);
        }
        if let Some(v) = get("profiler_direction") {
            c.profiler_direction = ProfilerDirection::from_ini_text(v);
        }
        if let Some(v) = get("renderdoc_enabled") {
            c.renderdoc_enabled = decode_bool(v);
        }
        if let Some(v) = get("bvh_stub_enabled") {
            c.bvh_stub_enabled = decode_bool(v);
        }
        if let Some(v) = get("host_input_mapping") {
            c.host_input_mapping = decode_string_list(v);
        }
        if let Some(v) = get("elf") {
            c.elf = decode_string(v);
        } else {
            c.elf = "eboot.bin".to_string();
        }
        c
    }
}

/// One entry from the `[GameConfigurations]` array — a per-game override,
/// keyed by `game_path` exactly like the Qt launcher's `m_custom_infos` map.
pub type GameOverride = Configuration;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KytyConfig {
    pub settings_file: String,
    pub game_dirs: Vec<String>,
    pub global: Configuration,
    pub game_overrides: Vec<GameOverride>,
    /// Physical-gamepad remap (`Control=SdlButtonOrAxisName`) and stick
    /// deadzone — Kyty-Launcher-only additions with no Qt-launcher UI, kept
    /// in their own `[KytyLauncherGamepad]` section rather than
    /// `[GlobalConfiguration]`/`[GameConfigurations]`: those two sections get
    /// wholesale `s->remove()`'d and rewritten by `ConfigurationListWidget::
    /// WriteSettings` on every Qt-launcher save, which would silently delete
    /// any key here Qt's own `Configuration` struct doesn't know about.
    /// Global rather than per-game, matching how the existing keyboard/mouse
    /// `host_input_mapping` already behaves in practice — `MainDialogPrivate::
    /// Run()` overwrites a launched game's mapping with the global one
    /// regardless of its own per-game value.
    pub gamepad_keymap: Vec<String>,
    pub gamepad_deadzone: f64,
}

const CONF_FILE_NAME: &str = "Kyty.ini";
static BACKED_UP_THIS_SESSION: AtomicBool = AtomicBool::new(false);

/// Mirrors `ConfigurationListWidget::ReadSettings`'s file resolution: a
/// `Kyty.ini` next to the current working directory wins (portable install),
/// otherwise the per-user path Qt's `QSettings::UserScope` resolves to for
/// org "Kyty" / app "Kyty" on Linux (`~/.config/Kyty/Kyty.ini`).
pub fn resolve_settings_path() -> PathBuf {
    let local = PathBuf::from(".").join(CONF_FILE_NAME);
    if local.is_file() {
        return local.canonicalize().unwrap_or(local);
    }
    dirs::config_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("Kyty")
        .join(CONF_FILE_NAME)
}

pub fn load(path: &Path) -> KytyConfig {
    let text = fs::read_to_string(path).unwrap_or_default();
    let doc = IniDocument::parse(&text);

    let game_dirs = doc
        .get("Launcher", "game_dirs")
        .map(decode_string_list)
        .unwrap_or_default();

    let global = Configuration::read_from(&doc, "GlobalConfiguration", "");

    let size = doc
        .get("GameConfigurations", "size")
        .map(|v| decode_int(v, 0))
        .unwrap_or(0)
        .max(0) as usize;

    let mut game_overrides = Vec::with_capacity(size);
    for i in 1..=size {
        let prefix = array_key(i, "");
        let cfg = Configuration::read_from(&doc, "GameConfigurations", &prefix);
        if !cfg.game_path.is_empty() {
            game_overrides.push(cfg);
        }
    }

    let gamepad_keymap = doc
        .get("KytyLauncherGamepad", "keymap")
        .map(decode_string_list)
        .unwrap_or_default();
    let gamepad_deadzone = doc
        .get("KytyLauncherGamepad", "deadzone")
        .map(|v| decode_f64(v, 0.0))
        .unwrap_or(0.0);

    KytyConfig {
        settings_file: path.display().to_string(),
        game_dirs,
        global,
        game_overrides,
        gamepad_keymap,
        gamepad_deadzone,
    }
}

pub fn save(path: &Path, config: &KytyConfig) -> std::io::Result<()> {
    let existing = fs::read_to_string(path).unwrap_or_default();
    let mut doc = IniDocument::parse(&existing);

    // Back up the pre-existing file once per process, before this app's
    // first write of the session touches it.
    if !existing.is_empty() && !BACKED_UP_THIS_SESSION.swap(true, Ordering::SeqCst) {
        let _ = fs::write(path.with_extension("ini.bak"), &existing);
    }

    doc.set("Launcher", "game_dirs", encode_string_list(&config.game_dirs));

    doc.remove_section("GlobalConfiguration");
    doc.ensure_section("GlobalConfiguration");
    config.global.write_into(&mut doc, "GlobalConfiguration", "");

    doc.remove_section("GameConfigurations");
    doc.ensure_section("GameConfigurations");
    for (i, game) in config.game_overrides.iter().enumerate() {
        let prefix = array_key(i + 1, "");
        game.write_into(&mut doc, "GameConfigurations", &prefix);
    }
    doc.set(
        "GameConfigurations",
        "size",
        encode_int(config.game_overrides.len() as i64),
    );

    // Own section, never wholesale-removed: safe to touch even though the
    // Qt launcher never will. Omitted entirely at defaults, matching how the
    // real Kyty.ini has no red_zone_protection_enabled key at all on
    // non-Windows builds — a user who never touches gamepad settings gets a
    // file indistinguishable from one the Qt launcher wrote.
    if config.gamepad_keymap.is_empty() && config.gamepad_deadzone == 0.0 {
        doc.remove_section("KytyLauncherGamepad");
    } else {
        doc.set("KytyLauncherGamepad", "keymap", encode_string_list(&config.gamepad_keymap));
        doc.set("KytyLauncherGamepad", "deadzone", encode_f64(config.gamepad_deadzone));
    }

    let text = doc.serialize();
    let tmp_path = path.with_extension("ini.tmp");
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&tmp_path, text)?;
    fs::rename(&tmp_path, path)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const REAL_KYTY_INI: &str = include_str!("../fixtures/real_kyty_empty.ini");
    const PROBE_ARRAY_INI: &str = include_str!("../fixtures/probe_array.ini");

    fn write_fixture(dir: &tempfile::TempDir, name: &str, content: &str) -> PathBuf {
        let path = dir.path().join(name);
        fs::write(&path, content).unwrap();
        path
    }

    #[test]
    fn resolution_round_trips_arbitrary_values() {
        let res = Resolution::new(2560, 1440);
        assert_eq!(res.as_ini_text(), "R2560X1440");
        assert_eq!(Resolution::from_ini_text("R2560X1440"), res);
        assert_eq!(res.dimensions(), (2560, 1440));
    }

    #[test]
    fn resolution_falls_back_to_default_on_malformed_text() {
        assert_eq!(Resolution::from_ini_text("garbage"), Resolution::default());
        assert_eq!(Resolution::from_ini_text("R1280"), Resolution::default());
        assert_eq!(Resolution::default(), Resolution::new(1280, 720));
    }

    #[test]
    fn load_then_save_real_file_is_byte_identical() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);

        let cfg = load(&path);
        assert_eq!(cfg.global.screen_resolution, Resolution::new(1280, 720));
        assert_eq!(cfg.global.printf_direction, LogDirection::Silent);
        assert!(cfg.game_dirs.is_empty());
        assert!(cfg.game_overrides.is_empty());

        save(&path, &cfg).unwrap();
        let after = fs::read_to_string(&path).unwrap();
        assert_eq!(after.trim_end(), REAL_KYTY_INI.trim_end());
    }

    #[test]
    fn load_then_save_preserves_unowned_geometry_blob() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);
        let cfg = load(&path);
        save(&path, &cfg).unwrap();

        let doc = IniDocument::parse(&fs::read_to_string(&path).unwrap());
        let geometry = doc.get("MainDialog", "geometry").unwrap();
        assert!(geometry.starts_with("@ByteArray("));
    }

    #[test]
    fn load_decodes_game_array_from_probe_fixture() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "probe.ini", PROBE_ARRAY_INI);
        let doc = IniDocument::parse(&fs::read_to_string(&path).unwrap());

        let g1 = Configuration::read_from(&doc, "GameConfigurations", "1\\");
        assert_eq!(g1.name, "Game 0");
        assert_eq!(g1.basedir, "/mnt/games/g0");
        assert_eq!(g1.vblank_frequency, 60);
        assert_eq!(g1.host_input_mapping, vec!["Cross=J".to_string(), "Circle=L,extra".to_string()]);

        let g2 = Configuration::read_from(&doc, "GameConfigurations", "2\\");
        assert_eq!(g2.name, "Game 1");
        assert_eq!(g2.vblank_frequency, 61);
    }

    #[test]
    fn round_trip_game_overrides_and_dirs() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);
        let mut cfg = load(&path);

        cfg.game_dirs = vec!["/home/user/Games".to_string(), "/mnt/ps5, backup".to_string()];
        let mut game = Configuration::default();
        game.name = "Astro's Playroom".to_string();
        game.basedir = "/home/user/Games/astro".to_string();
        game.game_path = "/home/user/Games/astro".to_string();
        game.custom_settings = true;
        game.fullscreen_enabled = true;
        game.screen_resolution = Resolution::new(1920, 1080);
        game.host_input_mapping = vec!["Cross=J".to_string()];
        cfg.game_overrides.push(game);

        save(&path, &cfg).unwrap();
        let reloaded = load(&path);

        assert_eq!(reloaded.game_dirs, cfg.game_dirs);
        assert_eq!(reloaded.game_overrides.len(), 1);
        assert_eq!(reloaded.game_overrides[0].name, "Astro's Playroom");
        assert_eq!(reloaded.game_overrides[0].screen_resolution, Resolution::new(1920, 1080));
        assert!(reloaded.game_overrides[0].fullscreen_enabled);
        assert_eq!(reloaded.game_overrides[0].host_input_mapping, vec!["Cross=J".to_string()]);

        // Saving twice more should be a fixed point.
        save(&path, &reloaded).unwrap();
        let text_a = fs::read_to_string(&path).unwrap();
        save(&path, &load(&path)).unwrap();
        let text_b = fs::read_to_string(&path).unwrap();
        assert_eq!(text_a, text_b);
    }

    #[test]
    fn kyty_config_json_shape_is_camel_case() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);
        let cfg = load(&path);
        let json = serde_json::to_value(&cfg).unwrap();
        assert!(json.get("gameDirs").is_some());
        assert!(json.get("gameOverrides").is_some());
        assert!(json.get("settingsFile").is_some());
        assert!(json.get("gamepadKeymap").is_some());
        assert!(json.get("gamepadDeadzone").is_some());
        assert!(json.get("game_dirs").is_none());
    }

    #[test]
    fn json_shape_matches_the_frontend_contract() {
        // Locks in the exact wire format the TypeScript `types.ts` depends
        // on: camelCase field names (via #[serde(rename_all)]) and enum
        // values spelled as ini text, never the raw Rust identifier (in
        // particular, ShaderOptimizationType::None_'s trailing underscore
        // must never leak into JSON).
        let mut cfg = Configuration::default();
        cfg.game_path = "/games/Astro".to_string();
        cfg.profiler_direction = ProfilerDirection::None_;
        let json: serde_json::Value = serde_json::to_value(&cfg).unwrap();

        assert_eq!(json["gamePath"], "/games/Astro");
        assert_eq!(json["screenResolution"], "R1280X720");
        assert_eq!(json["shaderOptimizationType"], "Performance");
        assert_eq!(json["profilerDirection"], "None");
        assert_eq!(json["vblankFrequency"], 60);

        let round_tripped: Configuration = serde_json::from_value(json).unwrap();
        assert_eq!(round_tripped.profiler_direction, ProfilerDirection::None_);
    }

    #[test]
    fn gamepad_section_omitted_at_defaults_and_removed_on_reset() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);
        let cfg = load(&path);
        save(&path, &cfg).unwrap();
        assert!(!fs::read_to_string(&path).unwrap().contains("KytyLauncherGamepad"));

        let mut with_gamepad = load(&path);
        with_gamepad.gamepad_deadzone = 0.1;
        save(&path, &with_gamepad).unwrap();
        assert!(fs::read_to_string(&path).unwrap().contains("KytyLauncherGamepad"));

        let mut reset = load(&path);
        reset.gamepad_deadzone = 0.0;
        save(&path, &reset).unwrap();
        assert!(!fs::read_to_string(&path).unwrap().contains("KytyLauncherGamepad"));
    }

    #[test]
    fn gamepad_keymap_and_deadzone_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);
        let mut cfg = load(&path);
        assert!(cfg.gamepad_keymap.is_empty());
        assert_eq!(cfg.gamepad_deadzone, 0.0);

        cfg.gamepad_keymap = vec!["Cross=a".to_string(), "LeftStickX=leftx".to_string()];
        cfg.gamepad_deadzone = 0.15;
        save(&path, &cfg).unwrap();

        let reloaded = load(&path);
        assert_eq!(reloaded.gamepad_keymap, cfg.gamepad_keymap);
        assert_eq!(reloaded.gamepad_deadzone, 0.15);
    }

    #[test]
    fn gamepad_section_survives_a_qt_style_global_and_game_section_wipe() {
        // Regression guard: ConfigurationListWidget::WriteSettings on the Qt
        // side does `s->remove("GlobalConfiguration")` and
        // `s->remove("GameConfigurations")` before rewriting them — it must
        // never take `[KytyLauncherGamepad]` down with it.
        let dir = tempfile::tempdir().unwrap();
        let path = write_fixture(&dir, "Kyty.ini", REAL_KYTY_INI);
        let mut cfg = load(&path);
        cfg.gamepad_keymap = vec!["Circle=b".to_string()];
        cfg.gamepad_deadzone = 0.2;
        save(&path, &cfg).unwrap();

        // Simulate the Qt launcher's own save: wipe + rewrite only the
        // sections it knows about, exactly as ConfigurationListWidget does.
        let mut doc = IniDocument::parse(&fs::read_to_string(&path).unwrap());
        doc.remove_section("GlobalConfiguration");
        doc.ensure_section("GlobalConfiguration");
        Configuration::default().write_into(&mut doc, "GlobalConfiguration", "");
        doc.remove_section("GameConfigurations");
        doc.ensure_section("GameConfigurations");
        doc.set("GameConfigurations", "size", "0".to_string());
        fs::write(&path, doc.serialize()).unwrap();

        let reloaded = load(&path);
        assert_eq!(reloaded.gamepad_keymap, vec!["Circle=b".to_string()]);
        assert_eq!(reloaded.gamepad_deadzone, 0.2);
    }

    #[test]
    fn console_language_out_of_range_falls_back_to_default() {
        let mut doc = IniDocument::default();
        doc.set("GlobalConfiguration", "console_language", "99".to_string());
        let cfg = Configuration::read_from(&doc, "GlobalConfiguration", "");
        assert_eq!(cfg.console_language, DEFAULT_CONSOLE_LANGUAGE);
    }
}
