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
        let status = value
            .get("status")
            .and_then(|v| v.as_str())
            .map(GameStatus::from_text)
            .unwrap_or_default();
        let comment = value.get("comment").and_then(|v| v.as_str()).unwrap_or_default().to_string();
        entries.insert(title_id, CompatibilityEntry { status, comment });
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
