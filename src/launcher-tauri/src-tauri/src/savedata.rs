//! `_SaveData/<title_id>` discovery and removal — Rust port of
//! `GetSaveDataDirs` / `remove_save_data` in `configurationListWidget.cpp`.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

const SAVE_DATA_DIR: &str = "_SaveData";

/// The four roots the Qt launcher checks: cwd, app binary dir, and each of
/// their parents.
fn candidate_roots(app_binary_dir: &Path) -> Vec<PathBuf> {
    let mut roots = vec![];
    if let Ok(cwd) = std::env::current_dir() {
        if let Some(parent) = cwd.parent() {
            roots.push(parent.to_path_buf());
        }
        roots.push(cwd);
    }
    if let Some(parent) = app_binary_dir.parent() {
        roots.push(parent.to_path_buf());
    }
    roots.push(app_binary_dir.to_path_buf());
    roots
}

pub fn find_save_data_dirs(app_binary_dir: &Path, title_id: &str) -> Vec<String> {
    let title_id = title_id.trim();
    if title_id.is_empty() {
        return vec![];
    }

    let mut seen = HashSet::new();
    let mut dirs = vec![];
    for root in candidate_roots(app_binary_dir) {
        let path = root.join(SAVE_DATA_DIR).join(title_id);
        if !path.is_dir() {
            continue;
        }
        let canonical = path.canonicalize().unwrap_or_else(|_| path.clone());
        if seen.insert(canonical.to_string_lossy().to_string()) {
            dirs.push(path.to_string_lossy().to_string());
        }
    }
    dirs
}

pub fn remove_save_data(dirs: &[String]) -> Vec<String> {
    let mut failed = vec![];
    for dir in dirs {
        if Path::new(dir).is_dir() && std::fs::remove_dir_all(dir).is_err() {
            failed.push(dir.clone());
        }
    }
    failed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_save_dir_next_to_app_binary() {
        let dir = tempfile::tempdir().unwrap();
        let app_dir = dir.path().join("install");
        std::fs::create_dir_all(app_dir.join("_SaveData/PPSA01234")).unwrap();

        let dirs = find_save_data_dirs(&app_dir, "PPSA01234");
        assert_eq!(dirs.len(), 1);
        assert!(dirs[0].contains("PPSA01234"));
    }

    #[test]
    fn empty_title_id_finds_nothing() {
        let dir = tempfile::tempdir().unwrap();
        assert!(find_save_data_dirs(dir.path(), "").is_empty());
    }

    #[test]
    fn remove_save_data_deletes_existing_dirs() {
        let dir = tempfile::tempdir().unwrap();
        let save_dir = dir.path().join("_SaveData/PPSA01234");
        std::fs::create_dir_all(&save_dir).unwrap();
        std::fs::write(save_dir.join("save.bin"), b"data").unwrap();

        let failed = remove_save_data(&[save_dir.to_string_lossy().to_string()]);
        assert!(failed.is_empty());
        assert!(!save_dir.exists());
    }
}
