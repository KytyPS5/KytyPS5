//! In-app folder/file browser backing `FolderBrowserModal.tsx` and
//! `ArtPickerModal.tsx`. No native file-dialog plugin dependency:
//! picking an image for game art works the same way as picking a game
//! folder, just with `file_extensions` set so matching files are listed
//! alongside subfolders and can be selected.

use serde::Serialize;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BrowseEntry {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
    /// True when the folder directly contains an `eboot.bin` — a quick
    /// visual hint while browsing to a game library root.
    pub looks_like_game: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BrowseResult {
    pub path: String,
    pub parent: Option<String>,
    pub home: String,
    pub entries: Vec<BrowseEntry>,
}

/// `file_extensions`, when given, also lists files whose extension matches
/// (case-insensitively) alongside subfolders — used by the art picker to
/// browse for an image instead of a folder.
pub fn browse_folder(path: Option<&str>, file_extensions: Option<&[String]>) -> Result<BrowseResult, String> {
    let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("/"));
    let target = match path {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => home.clone(),
    };

    let target = target.canonicalize().unwrap_or(target);
    if !target.is_dir() {
        return Err(format!("{} is not a folder.", target.display()));
    }

    let wanted_ext = |p: &Path| -> bool {
        let Some(exts) = file_extensions else { return false };
        let Some(ext) = p.extension().and_then(|e| e.to_str()) else { return false };
        exts.iter().any(|e| e.eq_ignore_ascii_case(ext))
    };

    let mut entries: Vec<BrowseEntry> = std::fs::read_dir(&target)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok())
        .filter(|e| !e.file_name().to_string_lossy().starts_with('.'))
        .filter_map(|e| {
            let path = e.path();
            if path.is_dir() {
                Some(BrowseEntry {
                    name: e.file_name().to_string_lossy().to_string(),
                    looks_like_game: path.join("eboot.bin").is_file(),
                    is_dir: true,
                    path: path.to_string_lossy().to_string(),
                })
            } else if wanted_ext(&path) {
                Some(BrowseEntry {
                    name: e.file_name().to_string_lossy().to_string(),
                    looks_like_game: false,
                    is_dir: false,
                    path: path.to_string_lossy().to_string(),
                })
            } else {
                None
            }
        })
        .collect();
    entries.sort_by(|a, b| match (a.is_dir, b.is_dir) {
        (true, false) => std::cmp::Ordering::Less,
        (false, true) => std::cmp::Ordering::Greater,
        _ => a.name.to_lowercase().cmp(&b.name.to_lowercase()),
    });

    let parent = parent_of(&target).map(|p| p.to_string_lossy().to_string());

    Ok(BrowseResult {
        path: target.to_string_lossy().to_string(),
        parent,
        home: home.to_string_lossy().to_string(),
        entries,
    })
}

fn parent_of(path: &Path) -> Option<PathBuf> {
    let parent = path.parent()?;
    (parent != path).then(|| parent.to_path_buf())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists_subdirectories_and_flags_games() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("Astro")).unwrap();
        std::fs::write(dir.path().join("Astro/eboot.bin"), b"x").unwrap();
        std::fs::create_dir_all(dir.path().join("Empty")).unwrap();
        std::fs::write(dir.path().join("not_a_dir.txt"), b"x").unwrap();

        let result = browse_folder(Some(dir.path().to_str().unwrap()), None).unwrap();
        assert_eq!(result.entries.len(), 2);
        let astro = result.entries.iter().find(|e| e.name == "Astro").unwrap();
        assert!(astro.looks_like_game);
        let empty = result.entries.iter().find(|e| e.name == "Empty").unwrap();
        assert!(!empty.looks_like_game);
    }

    #[test]
    fn rejects_a_file_path() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("f.txt");
        std::fs::write(&file, b"x").unwrap();
        assert!(browse_folder(Some(file.to_str().unwrap()), None).is_err());
    }

    #[test]
    fn file_extensions_filter_includes_matching_files_dirs_first() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("Sub")).unwrap();
        std::fs::write(dir.path().join("cover.png"), b"x").unwrap();
        std::fs::write(dir.path().join("readme.txt"), b"x").unwrap();
        std::fs::write(dir.path().join("Cover2.PNG"), b"x").unwrap();

        let exts = vec!["png".to_string(), "jpg".to_string()];
        let result = browse_folder(Some(dir.path().to_str().unwrap()), Some(&exts)).unwrap();

        let names: Vec<_> = result.entries.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, vec!["Sub", "cover.png", "Cover2.PNG"]);
        assert!(result.entries[0].is_dir);
        assert!(!result.entries[1].is_dir);
    }
}
