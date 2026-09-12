//! Records the first time each game's path was seen by a scan, so the
//! library can offer a "date added" sort. Pure Kyty-Launcher addition (the
//! Qt launcher has no equivalent and no such concept exists in `param.json`
//! or anywhere else on disk), so it lives in its own JSON file rather than
//! `Kyty.ini`, exactly like `playtime.rs`.

use std::collections::HashMap;
use std::path::Path;

/// `game_path -> unix ms of the first scan that ever saw it`.
pub type FirstSeen = HashMap<String, u64>;

const FILE_NAME: &str = "first_seen.json";

fn path(app_data_dir: &Path) -> std::path::PathBuf {
    app_data_dir.join(FILE_NAME)
}

fn load(app_data_dir: &Path) -> FirstSeen {
    std::fs::read_to_string(path(app_data_dir))
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save(app_data_dir: &Path, seen: &FirstSeen) -> std::io::Result<()> {
    std::fs::create_dir_all(app_data_dir)?;
    let text = serde_json::to_string_pretty(seen)?;
    std::fs::write(path(app_data_dir), text)
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Stamps every path in `paths` that has no record yet with the current
/// time, persists the result, and returns the full map. Never prunes a path
/// that drops out of `paths` (a game folder removed and re-added later keeps
/// its original date rather than jumping to the top of "newest first"), and
/// never restamps a path already on record.
pub fn record_and_load(app_data_dir: &Path, paths: &[String]) -> FirstSeen {
    let mut seen = load(app_data_dir);
    let mut changed = false;
    let stamp = now_ms();
    for path in paths {
        if !seen.contains_key(path) {
            seen.insert(path.clone(), stamp);
            changed = true;
        }
    }
    if changed {
        let _ = save(app_data_dir, &seen);
    }
    seen
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn new_paths_get_stamped() {
        let dir = tempfile::tempdir().unwrap();
        let seen = record_and_load(dir.path(), &["/games/Astro".to_string()]);
        assert!(seen.get("/games/Astro").copied().unwrap_or(0) > 0);
    }

    #[test]
    fn existing_path_is_never_restamped() {
        let dir = tempfile::tempdir().unwrap();
        let first = record_and_load(dir.path(), &["/games/Astro".to_string()]);
        let original = first["/games/Astro"];

        std::thread::sleep(std::time::Duration::from_millis(5));
        let second = record_and_load(dir.path(), &["/games/Astro".to_string()]);
        assert_eq!(second["/games/Astro"], original);
    }

    #[test]
    fn a_path_dropped_from_the_scan_keeps_its_record() {
        let dir = tempfile::tempdir().unwrap();
        record_and_load(dir.path(), &["/games/Astro".to_string(), "/games/Ashen".to_string()]);

        // Astro's folder is removed, so this scan only sees Ashen.
        let seen = record_and_load(dir.path(), &["/games/Ashen".to_string()]);
        assert!(seen.contains_key("/games/Astro"));
    }

    #[test]
    fn json_shape_is_a_flat_map_of_path_to_millis() {
        let dir = tempfile::tempdir().unwrap();
        record_and_load(dir.path(), &["/games/Astro".to_string()]);
        let text = std::fs::read_to_string(path(dir.path())).unwrap();
        let json: serde_json::Value = serde_json::from_str(&text).unwrap();
        assert!(json["/games/Astro"].as_u64().unwrap() > 0);
    }
}
