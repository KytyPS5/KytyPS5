//! Compatibility database — local edit mode or the remote community feed,
//! ported from `compatibilityDatabase.cpp`.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;

const FILE_NAME: &str = "compatibility_db.json";
const URL: &str = "https://kytyps5.github.io/data/compatibility.json";
const RETRY_COUNT: u32 = 3;
const RETRY_DELAY_MS: u64 = 750;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub enum GameStatus {
    Unknown,
    InGame,
    Logo,
    DoesntBoot,
    MainMenu,
}

impl Default for GameStatus {
    fn default() -> Self {
        GameStatus::Unknown
    }
}

impl GameStatus {
    fn from_text(text: &str) -> Self {
        match text.trim() {
            "InGame" | "In game" => GameStatus::InGame,
            "MainMenu" | "Main menu" => GameStatus::MainMenu,
            "Logo" => GameStatus::Logo,
            "DoesntBoot" | "Doesn't boot" => GameStatus::DoesntBoot,
            _ => GameStatus::Unknown,
        }
    }

    fn to_text(self) -> &'static str {
        match self {
            GameStatus::InGame => "InGame",
            GameStatus::MainMenu => "MainMenu",
            GameStatus::Logo => "Logo",
            GameStatus::DoesntBoot => "DoesntBoot",
            GameStatus::Unknown => "Unknown",
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CompatibilityEntry {
    pub status: GameStatus,
    #[serde(default)]
    pub comment: String,
    /// How many community reports back `status`. 0 for a locally-edited
    /// entry, which is the user's own opinion rather than a report count.
    #[serde(default)]
    pub reports: u32,
    /// The emulator build the reports were filed against, e.g.
    /// "KytyPS5-2026-08-16-bc2f077". Empty when the feed does not say.
    #[serde(default)]
    pub version: String,
    /// True when `status` came from this platform's own reports rather than
    /// the feed's cross-platform aggregate -- see `platform_key`.
    #[serde(default)]
    pub platform_specific: bool,
}

/// Which `platforms` sub-object of the community feed applies to this build.
///
/// The feed carries a per-OS breakdown next to its aggregate, and the two
/// disagree often enough to matter: a title reported InGame on Linux can be
/// DoesntBoot on Windows, and the aggregate hides that. #177 raised exactly
/// this ("be aware of the game compatibility across platforms ... probably
/// not, especially on macOS"), so prefer this platform's own reports and
/// keep the aggregate only as a fallback.
const fn platform_key() -> &'static str {
    #[cfg(windows)]
    {
        "windows"
    }
    #[cfg(target_os = "macos")]
    {
        "macos"
    }
    #[cfg(not(any(windows, target_os = "macos")))]
    {
        "linux"
    }
}

pub type CompatibilityMap = HashMap<String, CompatibilityEntry>;

fn title_key(title_id: &str) -> String {
    title_id.trim().to_uppercase()
}

fn parse(data: &str) -> Result<CompatibilityMap, String> {
    let raw: HashMap<String, serde_json::Value> =
        serde_json::from_str(data).map_err(|e| format!("Invalid compatibility JSON: {e}"))?;

    let mut entries = CompatibilityMap::new();
    for (key, value) in raw {
        let title_id = title_key(&key);
        if title_id.is_empty() {
            continue;
        }
        // This platform's own reports win over the cross-platform aggregate.
        // A locally-edited file has no "platforms" at all, so it falls
        // straight through to the top level, which is what it should do.
        let per_platform = value.get("platforms").and_then(|p| p.get(platform_key()));
        let platform_specific = per_platform.is_some();
        let source = per_platform.unwrap_or(&value);

        let status = source
            .get("status")
            .and_then(|v| v.as_str())
            .map(GameStatus::from_text)
            .unwrap_or_default();
        let comment = source.get("comment").and_then(|v| v.as_str()).unwrap_or_default().to_string();
        let reports = source.get("reports").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
        let version = source.get("version").and_then(|v| v.as_str()).unwrap_or_default().to_string();
        entries.insert(
            title_id,
            CompatibilityEntry { status, comment, reports, version, platform_specific },
        );
    }
    Ok(entries)
}

fn local_path(base_dir: &Path) -> std::path::PathBuf {
    base_dir.join(FILE_NAME)
}

pub fn load_local(base_dir: &Path) -> CompatibilityMap {
    let Ok(text) = std::fs::read_to_string(local_path(base_dir)) else {
        return CompatibilityMap::new();
    };
    parse(&text).unwrap_or_default()
}

pub fn save_local(base_dir: &Path, entries: &CompatibilityMap) -> std::io::Result<()> {
    let mut root = serde_json::Map::new();
    for (title_id, entry) in entries {
        root.insert(
            title_id.clone(),
            serde_json::json!({ "status": entry.status.to_text(), "comment": entry.comment }),
        );
    }
    let text = serde_json::to_string_pretty(&serde_json::Value::Object(root))?;
    std::fs::write(local_path(base_dir), text)
}

pub fn set_status(entries: &mut CompatibilityMap, title_id: &str, status: GameStatus) {
    let key = title_key(title_id);
    if key.is_empty() {
        return;
    }
    entries.entry(key).or_default().status = status;
}

pub fn set_comment(entries: &mut CompatibilityMap, title_id: &str, comment: String) {
    let key = title_key(title_id);
    if key.is_empty() {
        return;
    }
    entries.entry(key).or_default().comment = comment;
}

fn download_once() -> Result<CompatibilityMap, String> {
    let response = reqwest::blocking::Client::new()
        .get(URL)
        .header("User-Agent", "Kyty-Launcher")
        .timeout(std::time::Duration::from_secs(15))
        .send()
        .map_err(|e| e.to_string())?;
    let text = response.text().map_err(|e| e.to_string())?;
    parse(&text)
}

/// Fetches the remote community compatibility feed, retrying like
/// `CompatibilityDatabase::Download`.
pub fn download_remote() -> Result<CompatibilityMap, String> {
    let mut last_err = String::new();
    for attempt in 0..=RETRY_COUNT {
        if attempt > 0 {
            std::thread::sleep(std::time::Duration::from_millis(RETRY_DELAY_MS * attempt as u64));
        }
        match download_once() {
            Ok(map) => return Ok(map),
            Err(e) => last_err = e,
        }
    }
    Err(last_err)
}

#[allow(dead_code)]
pub fn find<'a>(entries: &'a CompatibilityMap, title_id: &str) -> Option<&'a CompatibilityEntry> {
    entries.get(&title_key(title_id))
}

#[allow(dead_code)]
pub fn cache_path(app_data_dir: &Path) -> std::path::PathBuf {
    app_data_dir.join(FILE_NAME)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_status_and_comment() {
        let json = r#"{"ppsa01234": {"status": "InGame", "comment": "runs great"}}"#;
        let entries = parse(json).unwrap();
        let entry = entries.get("PPSA01234").unwrap();
        assert_eq!(entry.status, GameStatus::InGame);
        assert_eq!(entry.comment, "runs great");
    }

    #[test]
    fn unknown_status_text_falls_back_to_unknown() {
        assert_eq!(GameStatus::from_text("Weird"), GameStatus::Unknown);
    }

    #[test]
    fn set_status_creates_entry_if_missing() {
        let mut entries = CompatibilityMap::new();
        set_status(&mut entries, "ppsa01234", GameStatus::MainMenu);
        assert_eq!(entries.get("PPSA01234").unwrap().status, GameStatus::MainMenu);
    }

    /// Shaped exactly like a real entry from the community feed.
    const FEED_ENTRY: &str = r#"{
      "PPSA01234": {
        "status": "InGame",
        "reports": 4,
        "comment": "4 reports",
        "platforms": {
          "windows": { "status": "DoesntBoot", "reports": 1, "comment": "1 report",
                       "version": "KytyPS5-2026-08-16-bc2f077" },
          "linux":   { "status": "InGame", "reports": 3, "comment": "3 reports",
                       "version": "KytyPS5-2026-08-16-bc2f077" },
          "macos":   { "status": "Logo", "reports": 1, "comment": "1 report",
                       "version": "KytyPS5-2026-08-16-bc2f077" }
        }
      }
    }"#;

    #[test]
    fn this_platforms_reports_win_over_the_aggregate() {
        let entry = parse(FEED_ENTRY).unwrap().remove("PPSA01234").unwrap();

        // The aggregate says InGame. Whatever this platform's own reports
        // say is what the user is shown instead -- the point of the split.
        let expected = if cfg!(windows) {
            GameStatus::DoesntBoot
        } else if cfg!(target_os = "macos") {
            GameStatus::Logo
        } else {
            GameStatus::InGame
        };
        assert_eq!(entry.status, expected);
        assert!(entry.platform_specific);
        assert_eq!(entry.version, "KytyPS5-2026-08-16-bc2f077");
    }

    #[test]
    fn entry_without_platforms_falls_back_to_the_aggregate() {
        let json = r#"{ "PPSA01234": { "status": "MainMenu", "reports": 2 } }"#;
        let entry = parse(json).unwrap().remove("PPSA01234").unwrap();

        assert_eq!(entry.status, GameStatus::MainMenu);
        assert_eq!(entry.reports, 2);
        // Nothing claimed this is a per-platform figure, so the UI must not
        // present it as one.
        assert!(!entry.platform_specific);
    }

    #[test]
    fn locally_edited_entries_are_never_platform_specific() {
        let dir = tempfile::tempdir().unwrap();
        let mut entries = CompatibilityMap::new();
        set_status(&mut entries, "PPSA01234", GameStatus::InGame);
        save_local(dir.path(), &entries).unwrap();

        let entry = load_local(dir.path()).remove("PPSA01234").unwrap();
        assert_eq!(entry.status, GameStatus::InGame);
        assert!(!entry.platform_specific);
        assert_eq!(entry.reports, 0, "a local edit is an opinion, not a report");
    }

    #[test]
    fn local_round_trip() {
        let dir = tempfile::tempdir().unwrap();

        let mut entries = CompatibilityMap::new();
        set_status(&mut entries, "PPSA01234", GameStatus::InGame);
        set_comment(&mut entries, "PPSA01234", "great".to_string());
        save_local(dir.path(), &entries).unwrap();

        let reloaded = load_local(dir.path());
        assert_eq!(reloaded.get("PPSA01234").unwrap().comment, "great");
    }
}
