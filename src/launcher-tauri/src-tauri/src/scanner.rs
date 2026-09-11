//! Game discovery — Rust port of the scanning half of
//! `ConfigurationListWidget::ScanGameDirectory` and its `param.json` readers
//! (`GetGameMetadata`, `GetLocalizedTitleName`, `GetFirmwareVersion`) from
//! `src/launcher/src/configurationListWidget.cpp`.

use crate::config::{Configuration, KytyConfig};
use regex::Regex;
use serde::Serialize;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

const EBOOT_NAME: &str = "eboot.bin";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScannedGame {
    pub game_path: String,
    pub basedir: String,
    pub name: String,
    pub title_id: String,
    pub game_version: String,
    pub firmware_ver: String,
    /// `sce_sys/icon0.png` if present — grid tile art.
    pub icon_path: Option<String>,
    /// `sce_sys/pic0.png` if present — detail-pane backdrop.
    pub backdrop_path: Option<String>,
}

struct GameMetadata {
    title_name: Option<String>,
    title_id: String,
    game_version: String,
    firmware_ver: String,
}

fn get_json_string(obj: &Value, key: &str) -> String {
    obj.get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .trim()
        .to_string()
}

fn get_localized_title_name(root: &Value) -> Option<String> {
    let localized = root.get("localizedParameters")?.as_object()?;
    if localized.is_empty() {
        return None;
    }

    let default_language = root
        .get("localizedParameters")
        .and_then(|v| v.get("defaultLanguage"))
        .and_then(Value::as_str)
        .unwrap_or("")
        .trim();

    if !default_language.is_empty() {
        if let Some(entry) = localized.get(default_language) {
            let title = get_json_string(entry, "titleName");
            if !title.is_empty() {
                return Some(title);
            }
        }
    }

    if let Some(entry) = localized.get("en-US") {
        let title = get_json_string(entry, "titleName");
        if !title.is_empty() {
            return Some(title);
        }
    }

    for (key, entry) in localized {
        if key == "defaultLanguage" {
            continue;
        }
        let title = get_json_string(entry, "titleName");
        if !title.is_empty() {
            return Some(title);
        }
    }

    None
}

fn firmware_version_regex() -> &'static Regex {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| Regex::new(r"^0[xX]([0-9]{6})[0-9A-Fa-f]{10}$").unwrap())
}

/// `requiredSystemSoftwareVersion` is a 16-hex-digit encoded value; the
/// first 6 decoded digits are `MMmmpp` (major, minor, patch), rendered as
/// `MM.mm` with a trailing `.pp` only when the patch isn't `00`.
fn get_firmware_version(root: &Value) -> String {
    let encoded = get_json_string(root, "requiredSystemSoftwareVersion");
    let Some(caps) = firmware_version_regex().captures(&encoded) else {
        return String::new();
    };
    let digits = &caps[1];
    let Ok(major) = digits[0..2].parse::<u32>() else {
        return String::new();
    };
    let minor = &digits[2..4];
    let patch = &digits[4..6];

    let mut version = format!("{major}.{minor}");
    if patch != "00" {
        version.push('.');
        version.push_str(patch);
    }
    version
}

fn get_game_metadata(param_file: &Path, fallback_name: &str) -> GameMetadata {
    let mut ret = GameMetadata {
        title_name: Some(fallback_name.to_string()),
        title_id: String::new(),
        game_version: String::new(),
        firmware_ver: String::new(),
    };

    let Ok(text) = std::fs::read_to_string(param_file) else {
        return ret;
    };
    let Ok(root) = serde_json::from_str::<Value>(&text) else {
        return ret;
    };
    if !root.is_object() {
        return ret;
    }

    if let Some(title) = get_localized_title_name(&root) {
        ret.title_name = Some(title);
    }

    ret.title_id = get_json_string(&root, "titleId");
    ret.game_version = {
        let v = get_json_string(&root, "appVersion");
        if v.is_empty() { get_json_string(&root, "contentVersion") } else { v }
    };
    ret.firmware_ver = get_firmware_version(&root);

    ret
}

fn existing_asset(game_dir: &Path, relative: &str) -> Option<String> {
    let path = game_dir.join(relative);
    path.is_file().then(|| path.to_string_lossy().to_string())
}

/// `canonicalize` resolves symlinks and relative segments, which is what
/// makes two spellings of the same game folder dedupe to one entry. On
/// Windows it also returns the verbatim `\\?\F:\Games` form, and that
/// spelling then reaches the emulator as `--game`, becomes the key in
/// playtime.json and shows up in the UI, so trim it straight back off.
fn normalize_dir(dir: &Path) -> PathBuf {
    let canonical = dir.canonicalize();
    let resolved = canonical.as_deref().unwrap_or(dir);
    PathBuf::from(crate::browse::display_path(resolved))
}

/// Scan every configured game folder for `eboot.bin`, mirroring the
/// breadth-first "stop descending once a game is found" walk in
/// `ScanGameDirectory`. Returns games sorted by name, matching the Qt
/// launcher's default `GAME_NAME_COLUMN` ascending sort.
pub fn scan_game_directories(game_dirs: &[String]) -> Vec<ScannedGame> {
    let mut found = Vec::new();
    let mut seen = HashSet::new();

    for root_str in game_dirs {
        let root = PathBuf::from(root_str);
        if root_str.trim().is_empty() || !root.is_dir() {
            continue;
        }

        let mut pending: Vec<PathBuf> = match std::fs::read_dir(&root) {
            Ok(entries) => entries
                .filter_map(|e| e.ok())
                .filter(|e| e.path().is_dir())
                .map(|e| e.path())
                .collect(),
            Err(_) => continue,
        };

        while let Some(game_dir) = pending.pop() {
            if game_dir.join(EBOOT_NAME).is_file() {
                let game_path = normalize_dir(&game_dir);
                let key = game_path.to_string_lossy().to_string();
                if !seen.insert(key) {
                    continue;
                }

                let dir_name = game_dir
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_default();
                let metadata =
                    get_game_metadata(&game_dir.join("sce_sys/param.json"), &dir_name);

                found.push(ScannedGame {
                    game_path: game_path.to_string_lossy().to_string(),
                    basedir: game_path.to_string_lossy().to_string(),
                    name: metadata.title_name.filter(|n| !n.is_empty()).unwrap_or(dir_name),
                    title_id: metadata.title_id,
                    game_version: metadata.game_version,
                    firmware_ver: metadata.firmware_ver,
                    icon_path: existing_asset(&game_dir, "sce_sys/icon0.png"),
                    backdrop_path: existing_asset(&game_dir, "sce_sys/pic0.png"),
                });
                continue;
            }

            if let Ok(entries) = std::fs::read_dir(&game_dir) {
                for entry in entries.filter_map(|e| e.ok()) {
                    if entry.path().is_dir() {
                        pending.push(entry.path());
                    }
                }
            }
        }
    }

    found.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    found
}

/// One entry in the merged library view: the effective per-game
/// `Configuration` (custom override if one exists, else the global defaults)
/// with the fields that are always freshly derived from the scan
/// (`name`/`title_id`/`game_version`/`firmware_ver`/`basedir`/`game_path`)
/// applied on top — mirrors `SetGameFiles` running after
/// `CopyFrom`/`CopyEmulatorSettingsFrom` in `ScanGameDirectory`.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameEntry {
    pub config: Configuration,
    pub icon_path: Option<String>,
    pub backdrop_path: Option<String>,
    /// Unix ms of the first scan that ever saw this `game_path`, from
    /// `first_seen::record_and_load` — 0 if somehow absent from that map.
    /// Drives the library's "date added" sort; see that module's own doc
    /// comment for why this can't be derived from anything on disk.
    pub first_seen_ms: u64,
}

pub fn merge_scanned_games(
    scanned: Vec<ScannedGame>,
    cfg: &KytyConfig,
    first_seen: &crate::first_seen::FirstSeen,
) -> Vec<GameEntry> {
    let overrides: HashMap<&str, &Configuration> =
        cfg.game_overrides.iter().map(|c| (c.game_path.as_str(), c)).collect();

    scanned
        .into_iter()
        .map(|game| {
            let mut config = match overrides.get(game.game_path.as_str()) {
                Some(custom) => (*custom).clone(),
                None => {
                    let mut c = Configuration::default();
                    c.copy_emulator_settings_from(&cfg.global);
                    c.custom_settings = false;
                    c
                }
            };

            let first_seen_ms = first_seen.get(&game.game_path).copied().unwrap_or(0);

            config.game_path = game.game_path;
            config.basedir = game.basedir;
            config.name = game.name;
            config.title_id = game.title_id;
            config.game_version = game.game_version;
            config.firmware_ver = game.firmware_ver;
            if config.elf.is_empty() {
                config.elf = "eboot.bin".to_string();
            }

            GameEntry {
                config,
                icon_path: game.icon_path,
                backdrop_path: game.backdrop_path,
                first_seen_ms,
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn make_game(dir: &Path, title_id: &str, name: &str) {
        fs::create_dir_all(dir.join("sce_sys")).unwrap();
        fs::write(dir.join("eboot.bin"), b"fake").unwrap();
        let param = serde_json::json!({
            "titleId": title_id,
            "appVersion": "1.03",
            "requiredSystemSoftwareVersion": "0x0550000000000000",
            "localizedParameters": {
                "defaultLanguage": "en-US",
                "en-US": { "titleName": name },
            }
        });
        fs::write(
            dir.join("sce_sys/param.json"),
            serde_json::to_string(&param).unwrap(),
        )
        .unwrap();
    }

    #[test]
    fn finds_game_under_one_level_of_nesting() {
        let tmp = tempfile::tempdir().unwrap();
        let game_dir = tmp.path().join("Astro's Playroom");
        make_game(&game_dir, "PPSA01234", "Astro's Playroom");

        let games = scan_game_directories(&[tmp.path().to_string_lossy().to_string()]);
        assert_eq!(games.len(), 1);
        assert_eq!(games[0].name, "Astro's Playroom");
        assert_eq!(games[0].title_id, "PPSA01234");
        assert_eq!(games[0].game_version, "1.03");
        assert_eq!(games[0].firmware_ver, "5.50");
    }

    #[test]
    fn falls_back_to_directory_name_without_param_json() {
        let tmp = tempfile::tempdir().unwrap();
        let game_dir = tmp.path().join("NoMetadataGame");
        fs::create_dir_all(&game_dir).unwrap();
        fs::write(game_dir.join(EBOOT_NAME), b"fake").unwrap();

        let games = scan_game_directories(&[tmp.path().to_string_lossy().to_string()]);
        assert_eq!(games.len(), 1);
        assert_eq!(games[0].name, "NoMetadataGame");
        assert!(games[0].title_id.is_empty());
    }

    #[test]
    fn does_not_descend_past_a_found_game() {
        let tmp = tempfile::tempdir().unwrap();
        let game_dir = tmp.path().join("Game");
        make_game(&game_dir, "PPSA00001", "Game");
        // A stray eboot.bin nested inside the already-found game directory
        // must not produce a second entry.
        fs::create_dir_all(game_dir.join("extra")).unwrap();
        fs::write(game_dir.join("extra").join(EBOOT_NAME), b"fake").unwrap();

        let games = scan_game_directories(&[tmp.path().to_string_lossy().to_string()]);
        assert_eq!(games.len(), 1);
    }

    #[test]
    fn firmware_version_with_zero_patch_omits_patch_segment() {
        let root: Value = serde_json::json!({ "requiredSystemSoftwareVersion": "0x0403000000000000" });
        assert_eq!(get_firmware_version(&root), "4.03");
    }

    #[test]
    fn game_entry_json_shape_is_camel_case() {
        let entry = GameEntry {
            config: Configuration::default(),
            icon_path: Some("/x/icon0.png".to_string()),
            backdrop_path: None,
            first_seen_ms: 12345,
        };
        let json = serde_json::to_value(&entry).unwrap();
        assert_eq!(json["iconPath"], "/x/icon0.png");
        assert!(json["backdropPath"].is_null());
        assert_eq!(json["firstSeenMs"], 12345);
        assert!(json.get("icon_path").is_none());
    }

    #[test]
    fn merge_applies_custom_override_but_keeps_fresh_scan_identity() {
        let tmp = tempfile::tempdir().unwrap();
        let game_dir = tmp.path().join("Astro's Playroom");
        make_game(&game_dir, "PPSA01234", "Astro's Playroom");
        let scanned = scan_game_directories(&[tmp.path().to_string_lossy().to_string()]);
        assert_eq!(scanned.len(), 1);

        let mut cfg = KytyConfig {
            settings_file: String::new(),
            game_dirs: vec![],
            global: Configuration::default(),
            game_overrides: vec![],
            gamepad_keymap: vec![],
            gamepad_deadzone: 0.0,
        };
        let mut custom = Configuration::default();
        custom.game_path = scanned[0].game_path.clone();
        custom.custom_settings = true;
        custom.fullscreen_enabled = true;
        custom.elf = "custom_boot.bin".to_string();
        // A stale name/title_id cached in a prior settings save — the merge
        // must prefer the fresh scan's identity fields regardless.
        custom.name = "stale cached name".to_string();
        custom.title_id = "STALE0000".to_string();
        cfg.game_overrides.push(custom);

        let merged = merge_scanned_games(scanned, &cfg, &crate::first_seen::FirstSeen::new());
        assert_eq!(merged.len(), 1);
        let entry = &merged[0].config;
        // Emulator settings come from the custom override.
        assert!(entry.fullscreen_enabled);
        assert_eq!(entry.elf, "custom_boot.bin");
        assert!(entry.custom_settings);
        // Identity fields always come from the fresh scan, never the
        // (possibly stale) cached custom entry.
        assert_eq!(entry.name, "Astro's Playroom");
        assert_eq!(entry.title_id, "PPSA01234");
    }

    #[test]
    fn merge_falls_back_to_global_defaults_without_override() {
        let tmp = tempfile::tempdir().unwrap();
        let game_dir = tmp.path().join("Game");
        make_game(&game_dir, "PPSA00001", "Game");
        let scanned = scan_game_directories(&[tmp.path().to_string_lossy().to_string()]);

        let mut global = Configuration::default();
        global.vblank_frequency = 30;
        let cfg = KytyConfig {
            settings_file: String::new(),
            game_dirs: vec![],
            global,
            game_overrides: vec![],
            gamepad_keymap: vec![],
            gamepad_deadzone: 0.0,
        };

        let merged = merge_scanned_games(scanned, &cfg, &crate::first_seen::FirstSeen::new());
        assert_eq!(merged[0].config.vblank_frequency, 30);
        assert!(!merged[0].config.custom_settings);
    }

    #[test]
    fn firmware_version_with_nonzero_patch_includes_it() {
        let root: Value = serde_json::json!({ "requiredSystemSoftwareVersion": "0x0403010000000000" });
        assert_eq!(get_firmware_version(&root), "4.03.01");
    }
}
