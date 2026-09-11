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
    /// True for the Windows drive list, which is a made-up level rather than
    /// a real directory: it has no path you could hand to the emulator, so
    /// the UI keeps its "select this folder" action disabled while it shows.
    pub is_virtual: bool,
}

/// Stand-in path for "the level above a drive root" on Windows, i.e. what
/// Explorer calls This PC. `C:\` has no parent, so without this there is no
/// way to get from one drive to another in the browser.
#[cfg(windows)]
pub const DRIVE_ROOT: &str = "::drives";

/// Trim a canonicalized path back to its ordinary spelling. `canonicalize`
/// on Windows hands back the verbatim `\\?\C:\Users\you` form, which is
/// correct but is not what anyone recognizes as their own path, and is not
/// a spelling every program accepts as an argument. UNC paths canonicalize
/// to `\\?\UNC\server\share` and come back as `\\server\share`.
///
/// Shared with scanner.rs, which canonicalizes game folders and would
/// otherwise carry the prefix into `--game`, into playtime.json's keys and
/// into the UI.
pub fn display_path(path: &Path) -> String {
    let text = path.to_string_lossy().to_string();
    #[cfg(windows)]
    {
        if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
            return format!(r"\\{rest}");
        }
        if let Some(rest) = text.strip_prefix(r"\\?\") {
            return rest.to_string();
        }
    }
    text
}

/// Every drive letter currently mounted, in the shape the browser lists
/// folders in. Probing A-Z with a `is_dir` check keeps this dependency-free;
/// the alternative is `GetLogicalDrives` through a winapi crate, which is a
/// lot of surface for 26 stat calls.
#[cfg(windows)]
fn drive_entries() -> Vec<BrowseEntry> {
    (b'A'..=b'Z')
        .map(|letter| format!("{}:\\", letter as char))
        .filter(|root| Path::new(root).is_dir())
        .map(|root| BrowseEntry {
            name: root.clone(),
            path: root,
            is_dir: true,
            looks_like_game: false,
        })
        .collect()
}

/// `file_extensions`, when given, also lists files whose extension matches
/// (case-insensitively) alongside subfolders — used by the art picker to
/// browse for an image instead of a folder.
pub fn browse_folder(path: Option<&str>, file_extensions: Option<&[String]>) -> Result<BrowseResult, String> {
    let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("/"));

    // The drive list, either asked for outright or as the Windows opening
    // view: a games library usually lives on some other drive than the one
    // the home folder is on, so starting at This PC saves walking up out of
    // `C:\Users\<name>` every time. If no drive can be listed at all, fall
    // through to the home folder rather than showing an empty picker.
    #[cfg(windows)]
    {
        let wants_drive_root = path == Some(DRIVE_ROOT) || path.map_or(true, |p| p.is_empty());
        if wants_drive_root {
            let entries = drive_entries();
            if !entries.is_empty() {
                return Ok(BrowseResult {
                    path: DRIVE_ROOT.to_string(),
                    parent: None,
                    home: display_path(&home),
                    entries,
                    is_virtual: true,
                });
            }
        }
    }

    let target = match path {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => home.clone(),
    };

    let target = target.canonicalize().unwrap_or(target);
    if !target.is_dir() {
        return Err(format!("{} is not a folder.", display_path(&target)));
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
                    path: display_path(&path),
                })
            } else if wanted_ext(&path) {
                Some(BrowseEntry {
                    name: e.file_name().to_string_lossy().to_string(),
                    looks_like_game: false,
                    is_dir: false,
                    path: display_path(&path),
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

    Ok(BrowseResult {
        path: display_path(&target),
        parent: parent_of(&target),
        home: display_path(&home),
        entries,
        is_virtual: false,
    })
}

fn parent_of(path: &Path) -> Option<String> {
    match path.parent() {
        Some(parent) if parent != path => Some(display_path(parent)),
        // Already at a filesystem root. On Windows that is a drive root, and
        // the level above it is the drive list; on Unix `/` really is the top.
        _ => {
            #[cfg(windows)]
            {
                Some(DRIVE_ROOT.to_string())
            }
            #[cfg(not(windows))]
            {
                None
            }
        }
    }
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
    #[cfg(windows)]
    fn display_path_trims_the_verbatim_prefix() {
        assert_eq!(display_path(Path::new(r"\\?\C:\Users\you")), r"C:\Users\you");
        assert_eq!(display_path(Path::new(r"\\?\UNC\server\share")), r"\\server\share");
        assert_eq!(display_path(Path::new(r"C:\Users\you")), r"C:\Users\you");
    }

    #[test]
    #[cfg(windows)]
    fn drive_root_is_the_parent_of_a_drive() {
        assert_eq!(parent_of(Path::new(r"C:\")).as_deref(), Some(DRIVE_ROOT));
        assert_eq!(parent_of(Path::new(r"C:\Users")).as_deref(), Some(r"C:\"));
    }

    #[test]
    #[cfg(windows)]
    fn opens_on_the_drive_list_when_no_path_is_given() {
        for path in [None, Some("")] {
            let result = browse_folder(path, None).unwrap();
            assert!(result.is_virtual, "{path:?} should open This PC, not the home folder");
            assert!(!result.entries.is_empty());
        }
    }

    #[test]
    #[cfg(not(windows))]
    fn opens_on_the_home_folder_when_no_path_is_given() {
        let result = browse_folder(None, None).unwrap();
        assert!(!result.is_virtual);
        assert_eq!(result.path, result.home);
    }

    #[test]
    #[cfg(windows)]
    fn drive_list_is_virtual_and_lists_mounted_drives() {
        let result = browse_folder(Some(DRIVE_ROOT), None).unwrap();
        assert!(result.is_virtual);
        assert!(result.parent.is_none());
        assert!(!result.entries.is_empty(), "expected at least one mounted drive");
        assert!(result.entries.iter().all(|e| e.is_dir));
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
