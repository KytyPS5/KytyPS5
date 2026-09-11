//! Local play-history tracking: last played, play count, and accumulated
//! playtime per game. Pure Kyty-Launcher addition (the Qt launcher has no
//! equivalent), so it lives in its own JSON file rather than `Kyty.ini` —
//! nothing here should ever need to round-trip through Qt's `QSettings`.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::Path;

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlayStats {
    /// Unix milliseconds of the most recent launch.
    pub last_played_ms: u64,
    pub play_count: u64,
    pub total_seconds: u64,
}

pub type PlayHistory = HashMap<String, PlayStats>;

const FILE_NAME: &str = "playtime.json";

fn path(app_data_dir: &Path) -> std::path::PathBuf {
    app_data_dir.join(FILE_NAME)
}

pub fn load(app_data_dir: &Path) -> PlayHistory {
    std::fs::read_to_string(path(app_data_dir))
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

pub fn save(app_data_dir: &Path, history: &PlayHistory) -> std::io::Result<()> {
    std::fs::create_dir_all(app_data_dir)?;
    let text = serde_json::to_string_pretty(history)?;
    std::fs::write(path(app_data_dir), text)
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Call when a game starts: bumps `play_count` and `last_played_ms`.
pub fn record_start(app_data_dir: &Path, game_path: &str) -> std::io::Result<()> {
    let mut history = load(app_data_dir);
    let entry = history.entry(game_path.to_string()).or_default();
    entry.play_count += 1;
    entry.last_played_ms = now_ms();
    save(app_data_dir, &history)
}

/// Call when a game's process exits: adds the elapsed session length to
/// `total_seconds`.
pub fn record_stop(app_data_dir: &Path, game_path: &str, session_seconds: u64) -> std::io::Result<()> {
    let mut history = load(app_data_dir);
    let entry = history.entry(game_path.to_string()).or_default();
    entry.total_seconds += session_seconds;
    save(app_data_dir, &history)
}

/// Games with a recorded play, newest first — the "Continue Playing" rail.
/// The frontend does this join itself (it needs each game's name/art from
/// `GameEntry`, which this module has no access to); kept here, tested, as
/// the reference implementation and for any future non-UI consumer.
#[allow(dead_code)]
pub fn recently_played(history: &PlayHistory, limit: usize) -> Vec<(String, PlayStats)> {
    let mut entries: Vec<_> = history.iter().map(|(k, v)| (k.clone(), *v)).collect();
    entries.sort_by(|a, b| b.1.last_played_ms.cmp(&a.1.last_played_ms));
    entries.truncate(limit);
    entries
}

#[derive(Debug, Clone, Copy, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LibraryStats {
    pub total_games_played: u64,
    pub total_play_count: u64,
    pub total_seconds: u64,
}

pub fn library_stats(history: &PlayHistory) -> LibraryStats {
    LibraryStats {
        total_games_played: history.len() as u64,
        total_play_count: history.values().map(|s| s.play_count).sum(),
        total_seconds: history.values().map(|s| s.total_seconds).sum(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn record_start_increments_count_and_sets_last_played() {
        let dir = tempfile::tempdir().unwrap();
        record_start(dir.path(), "/games/Astro").unwrap();
        record_start(dir.path(), "/games/Astro").unwrap();

        let history = load(dir.path());
        let entry = history.get("/games/Astro").unwrap();
        assert_eq!(entry.play_count, 2);
        assert!(entry.last_played_ms > 0);
    }

    #[test]
    fn record_stop_accumulates_seconds_across_sessions() {
        let dir = tempfile::tempdir().unwrap();
        record_start(dir.path(), "/games/Astro").unwrap();
        record_stop(dir.path(), "/games/Astro", 120).unwrap();
        record_stop(dir.path(), "/games/Astro", 30).unwrap();

        let history = load(dir.path());
        assert_eq!(history.get("/games/Astro").unwrap().total_seconds, 150);
    }

    #[test]
    fn recently_played_sorts_newest_first_and_respects_limit() {
        let mut history = PlayHistory::new();
        history.insert("a".into(), PlayStats { last_played_ms: 100, ..Default::default() });
        history.insert("b".into(), PlayStats { last_played_ms: 300, ..Default::default() });
        history.insert("c".into(), PlayStats { last_played_ms: 200, ..Default::default() });

        let top = recently_played(&history, 2);
        assert_eq!(top.iter().map(|(k, _)| k.clone()).collect::<Vec<_>>(), vec!["b", "c"]);
    }

    #[test]
    fn json_shape_is_camel_case() {
        let stats = PlayStats { last_played_ms: 42, play_count: 3, total_seconds: 99 };
        let json = serde_json::to_value(stats).unwrap();
        assert_eq!(json["lastPlayedMs"], 42);
        assert_eq!(json["playCount"], 3);
        assert_eq!(json["totalSeconds"], 99);
    }

    #[test]
    fn library_stats_sums_across_games() {
        let mut history = PlayHistory::new();
        history.insert("a".into(), PlayStats { play_count: 3, total_seconds: 100, ..Default::default() });
        history.insert("b".into(), PlayStats { play_count: 2, total_seconds: 50, ..Default::default() });

        let stats = library_stats(&history);
        assert_eq!(stats.total_games_played, 2);
        assert_eq!(stats.total_play_count, 5);
        assert_eq!(stats.total_seconds, 150);
    }
}
