//! Per-game cover art. Three tiers, checked in this order by the frontend:
//! a user-supplied override (this module), the `sce_sys/icon0.png` /
//! `pic0.png` already bundled in the user's own game dump (`scanner.rs`),
//! or a generic placeholder tile drawn in CSS. There is no networked art
//! fetch yet — see `ART_SOURCES.md` for the PSN-entitlement-based design
//! this is left ready for (only fetch art for titles the user's own PSN
//! session shows as owned, mirroring how Lutris pulls Steam/GOG/Epic art
//! through the user's own service account rather than an unlicensed
//! scrape).

use std::path::{Path, PathBuf};

fn sanitize_key(key: &str) -> String {
    key.chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect()
}

fn art_dir(app_data_dir: &Path) -> PathBuf {
    app_data_dir.join("art")
}

/// Copy a user-picked image into the app's art store, keyed by `art_key`
/// (the game's `title_id` when known, else a sanitized `game_path`).
/// Overwrites any prior art for the same key. Returns the stored path.
pub fn set_game_art(
    app_data_dir: &Path,
    art_key: &str,
    source_path: &Path,
) -> std::io::Result<String> {
    let dir = art_dir(app_data_dir);
    std::fs::create_dir_all(&dir)?;

    let ext = source_path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("png")
        .to_lowercase();

    // Remove any previously stored art for this key under a different
    // extension before copying the new one in.
    for existing in ["png", "jpg", "jpeg", "webp", "gif"] {
        let stale = dir.join(format!("{}.{existing}", sanitize_key(art_key)));
        if stale.exists() {
            let _ = std::fs::remove_file(&stale);
        }
    }

    let dest = dir.join(format!("{}.{ext}", sanitize_key(art_key)));
    std::fs::copy(source_path, &dest)?;
    Ok(dest.to_string_lossy().to_string())
}

pub fn get_game_art(app_data_dir: &Path, art_key: &str) -> Option<String> {
    let dir = art_dir(app_data_dir);
    for ext in ["png", "jpg", "jpeg", "webp", "gif"] {
        let candidate = dir.join(format!("{}.{ext}", sanitize_key(art_key)));
        if candidate.is_file() {
            return Some(candidate.to_string_lossy().to_string());
        }
    }
    None
}

pub fn remove_game_art(app_data_dir: &Path, art_key: &str) -> std::io::Result<()> {
    let dir = art_dir(app_data_dir);
    for ext in ["png", "jpg", "jpeg", "webp", "gif"] {
        let candidate = dir.join(format!("{}.{ext}", sanitize_key(art_key)));
        if candidate.exists() {
            std::fs::remove_file(candidate)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn set_then_get_round_trips() {
        let app_dir = tempfile::tempdir().unwrap();
        let src_dir = tempfile::tempdir().unwrap();
        let src = src_dir.path().join("cover.png");
        std::fs::write(&src, b"fake png bytes").unwrap();

        let stored = set_game_art(app_dir.path(), "PPSA01234", &src).unwrap();
        assert!(stored.ends_with("PPSA01234.png"));
        assert_eq!(get_game_art(app_dir.path(), "PPSA01234"), Some(stored));
    }

    #[test]
    fn setting_a_new_extension_removes_the_old_file() {
        let app_dir = tempfile::tempdir().unwrap();
        let src_dir = tempfile::tempdir().unwrap();

        let png = src_dir.path().join("a.png");
        std::fs::write(&png, b"x").unwrap();
        set_game_art(app_dir.path(), "key", &png).unwrap();

        let jpg = src_dir.path().join("b.jpg");
        std::fs::write(&jpg, b"y").unwrap();
        let stored = set_game_art(app_dir.path(), "key", &jpg).unwrap();

        assert!(stored.ends_with("key.jpg"));
        assert!(!art_dir(app_dir.path()).join("key.png").exists());
    }

    #[test]
    fn missing_art_returns_none() {
        let app_dir = tempfile::tempdir().unwrap();
        assert_eq!(get_game_art(app_dir.path(), "NOPE"), None);
    }

    #[test]
    fn key_is_sanitized_for_path_safety() {
        let sanitized = sanitize_key("../../etc/passwd");
        assert!(!sanitized.contains('/'));
        assert!(!sanitized.contains(".."));
        assert_eq!(sanitized, "______etc_passwd");
    }
}
