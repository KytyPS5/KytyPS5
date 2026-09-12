//! `_Patches/<TITLEID>.json` read/write — Rust port of `patchesDialog.cpp`.

use serde::Serialize;
use serde_json::Value;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PatchEntry {
    pub name: String,
    pub enabled: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PatchStatus {
    pub patches: Vec<PatchEntry>,
    /// Human-readable status line matching the Qt dialog's messages (no
    /// file, invalid JSON, a "mods" list instead of "patches", etc.).
    pub message: String,
}

pub fn is_supported_title_id(title_id: &str) -> bool {
    title_id.trim().to_uppercase().starts_with("PPSA")
}

pub fn patch_plan_path(app_binary_dir: &Path, title_id: &str) -> PathBuf {
    app_binary_dir
        .join("_Patches")
        .join(format!("{}.json", title_id.trim().to_uppercase()))
}

pub fn load_patches(path: &Path) -> PatchStatus {
    let Ok(text) = std::fs::read_to_string(path) else {
        return PatchStatus {
            patches: vec![],
            message: format!("No local patch file: {}", path.display()),
        };
    };

    let Ok(root) = serde_json::from_str::<Value>(&text) else {
        return PatchStatus {
            patches: vec![],
            message: format!("Invalid patch JSON in {}.", path.display()),
        };
    };

    let patches: Vec<PatchEntry> = root
        .get("patches")
        .and_then(Value::as_array)
        .map(|arr| {
            arr.iter()
                .map(|p| PatchEntry {
                    name: p.get("name").and_then(Value::as_str).unwrap_or_default().to_string(),
                    enabled: p.get("enabled").and_then(Value::as_bool).unwrap_or(true),
                })
                .collect()
        })
        .unwrap_or_default();

    let message = if !patches.is_empty() {
        format!("Loaded {} patch(es) from {}.", patches.len(), path.display())
    } else if root.get("mods").is_some() {
        format!(
            "This file uses a \"mods\" list. Kyty expects a top-level \"patches\" array in {}.",
            path.display()
        )
    } else if root.get("patches").is_none() {
        format!("No \"patches\" array in {}.", path.display())
    } else {
        format!("Loaded 0 patch(es) from {}.", path.display())
    };

    PatchStatus { patches, message }
}

/// Apply the checked/unchecked state from the UI back onto the on-disk
/// `patches[]` array, preserving every other field in each patch object.
pub fn save_patches(path: &Path, enabled: &[bool]) -> Result<(), String> {
    let text = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let mut root: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;

    let Some(patches) = root.get_mut("patches").and_then(Value::as_array_mut) else {
        return Err("No \"patches\" array to save.".to_string());
    };
    for (patch, &is_enabled) in patches.iter_mut().zip(enabled) {
        if let Some(obj) = patch.as_object_mut() {
            obj.insert("enabled".to_string(), Value::Bool(is_enabled));
        }
    }

    let out = serde_json::to_string_pretty(&root).map_err(|e| e.to_string())?;
    std::fs::write(path, out).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn supported_title_ids_are_ppsa_prefixed() {
        assert!(is_supported_title_id("ppsa01234"));
        assert!(is_supported_title_id("PPSA01234"));
        assert!(!is_supported_title_id("CUSA01234"));
    }

    #[test]
    fn loads_patches_array() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("PPSA01234.json");
        std::fs::write(&path, r#"{"patches":[{"name":"Infinite HP","enabled":true}]}"#).unwrap();

        let status = load_patches(&path);
        assert_eq!(status.patches.len(), 1);
        assert_eq!(status.patches[0].name, "Infinite HP");
        assert!(status.message.starts_with("Loaded 1 patch"));
    }

    #[test]
    fn flags_a_mods_only_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("PPSA01234.json");
        std::fs::write(&path, r#"{"mods":[{"name":"x"}]}"#).unwrap();

        let status = load_patches(&path);
        assert!(status.patches.is_empty());
        assert!(status.message.contains("\"mods\" list"));
    }

    #[test]
    fn save_preserves_other_fields() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("PPSA01234.json");
        std::fs::write(
            &path,
            r#"{"patches":[{"name":"A","enabled":true,"offset":"0x1000"}]}"#,
        )
        .unwrap();

        save_patches(&path, &[false]).unwrap();

        let saved: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        let patch = &saved["patches"][0];
        assert_eq!(patch["enabled"], false);
        assert_eq!(patch["offset"], "0x1000");
    }
}
