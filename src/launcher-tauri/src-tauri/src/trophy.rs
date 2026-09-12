//! Trophy viewer — Rust port of `trophyViewerDialog.cpp`'s UCP container
//! reader and `tropconf.json`/`tropmeta_*.json` extraction.
//!
//! A `.ucp` file is a small custom archive: a 0x40-byte header (magic,
//! version, declared size, file count, TOC offset), then a table of
//! contents of fixed 0x40-byte entries (each a 0x20-byte name plus 8-byte
//! offset/size), 0x20 bytes past the TOC offset itself.

use serde::Serialize;
use serde_json::Value;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

const UCP_MAGIC: u32 = 0xb228_c60a;
const UCP_VERSION: u32 = 1;
const UCP_HEADER_LEN: usize = 0x40;
const UCP_TOC_SKIP: usize = 0x20;
const UCP_ENTRY_LEN: usize = 0x40;
const UCP_NAME_LEN: usize = 0x20;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TrophyRow {
    pub id: String,
    pub name: String,
    pub detail: String,
    pub grade: String,
    pub grade_text: String,
    pub reward: String,
    pub hidden: bool,
    pub has_reward: bool,
    pub icon_path: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TrophySet {
    pub tab_title: String,
    pub trophies: Vec<TrophyRow>,
}

fn read_be32(data: &[u8], offset: usize) -> Option<u32> {
    data.get(offset..offset + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap()))
}

fn read_be64(data: &[u8], offset: usize) -> Option<u64> {
    data.get(offset..offset + 8).map(|b| u64::from_be_bytes(b.try_into().unwrap()))
}

fn read_fixed_string(data: &[u8], offset: usize, max_size: usize) -> Option<String> {
    let slice = data.get(offset..offset + max_size)?;
    let len = slice.iter().position(|&b| b == 0).unwrap_or(max_size);
    Some(String::from_utf8_lossy(&slice[..len]).trim().to_string())
}

/// Parses a `.ucp` container into `name (case-folded) -> bytes`.
fn read_ucp(path: &Path) -> Result<HashMap<String, Vec<u8>>, String> {
    let data = std::fs::read(path).map_err(|e| format!("Could not open {}: {e}", path.display()))?;
    let file_name = path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();

    if data.len() < UCP_HEADER_LEN {
        return Err(format!("{file_name} is too small to be a trophy package."));
    }
    let magic = read_be32(&data, 0x00).unwrap();
    if magic != UCP_MAGIC {
        return Err(format!("{file_name} has an invalid trophy package magic."));
    }
    let version = read_be32(&data, 0x04).unwrap();
    if version != UCP_VERSION {
        return Err(format!("{file_name} uses unsupported trophy package version {version}."));
    }
    let declared_size = read_be64(&data, 0x08).unwrap();
    if declared_size > data.len() as u64 {
        return Err(format!("{file_name} is truncated."));
    }

    let file_count = read_be32(&data, 0x10).unwrap() as u64;
    let toc_offset = read_be32(&data, 0x14).unwrap() as u64;
    let data_len = data.len() as u64;

    let table_size = UCP_TOC_SKIP as u64 + file_count * UCP_ENTRY_LEN as u64;
    if toc_offset > data_len || table_size > data_len - toc_offset {
        return Err(format!("{file_name} has an invalid table of contents."));
    }

    let mut files = HashMap::new();
    for i in 0..file_count {
        let entry_offset = (toc_offset + UCP_TOC_SKIP as u64 + i * UCP_ENTRY_LEN as u64) as usize;
        let Some(name) = read_fixed_string(&data, entry_offset, UCP_NAME_LEN) else {
            return Err(format!("{file_name} has a truncated table of contents."));
        };
        let offset = read_be64(&data, entry_offset + 0x20)
            .ok_or_else(|| format!("{file_name} has a truncated table of contents."))?;
        let size = read_be64(&data, entry_offset + 0x28)
            .ok_or_else(|| format!("{file_name} has a truncated table of contents."))?;

        if name.is_empty() {
            continue;
        }
        if offset > data_len || size > data_len - offset {
            return Err(format!("{file_name} has an invalid entry for {name}."));
        }

        let start = offset as usize;
        let end = start + size as usize;
        files.insert(name.to_lowercase(), data[start..end].to_vec());
    }

    Ok(files)
}

fn find_file<'a>(files: &'a HashMap<String, Vec<u8>>, name: &str) -> Option<&'a Vec<u8>> {
    files.get(&name.to_lowercase())
}

fn find_first_trophy_metadata_file(files: &HashMap<String, Vec<u8>>) -> Option<&Vec<u8>> {
    for (key, value) in files {
        if key.starts_with("tropmeta_") && key.ends_with(".json") {
            return Some(value);
        }
    }
    find_file(files, "tropmeta.json")
}

fn json_string(value: &Value) -> String {
    match value {
        Value::String(s) => s.clone(),
        Value::Number(n) => n.to_string(),
        Value::Bool(b) => b.to_string(),
        _ => String::new(),
    }
}

struct TrophyDefinition {
    id: String,
    grade: String,
    hidden: bool,
    has_reward: bool,
}

fn read_definitions(tropconf: &Value) -> Vec<TrophyDefinition> {
    tropconf
        .get("trophies")
        .and_then(Value::as_array)
        .map(|arr| {
            arr.iter()
                .filter_map(|v| {
                    let id = json_string(v.get("id").unwrap_or(&Value::Null)).trim().to_string();
                    if id.is_empty() {
                        return None;
                    }
                    Some(TrophyDefinition {
                        id,
                        grade: json_string(v.get("grade").unwrap_or(&Value::Null)).trim().to_string(),
                        hidden: v.get("hidden").and_then(Value::as_bool).unwrap_or(false),
                        has_reward: v.get("hasReward").and_then(Value::as_bool).unwrap_or(false),
                    })
                })
                .collect()
        })
        .unwrap_or_default()
}

struct TrophyText {
    name: String,
    detail: String,
    reward: String,
}

fn read_metadata(tropmeta: &Value) -> HashMap<String, TrophyText> {
    let mut ret = HashMap::new();
    let Some(trophies) = tropmeta
        .get("metadata")
        .and_then(|m| m.get("trophyMetadata"))
        .and_then(Value::as_array)
    else {
        return ret;
    };
    for v in trophies {
        let id = json_string(v.get("id").unwrap_or(&Value::Null)).trim().to_string();
        if id.is_empty() {
            continue;
        }
        ret.insert(
            id,
            TrophyText {
                name: json_string(v.get("name").unwrap_or(&Value::Null)).trim().to_string(),
                detail: json_string(v.get("detail").unwrap_or(&Value::Null)).trim().to_string(),
                reward: json_string(v.get("reward").unwrap_or(&Value::Null)).trim().to_string(),
            },
        );
    }
    ret
}

fn grade_to_text(grade: &str) -> String {
    match grade {
        "P" => "Platinum".to_string(),
        "G" => "Gold".to_string(),
        "S" => "Silver".to_string(),
        "B" => "Bronze".to_string(),
        other => other.to_string(),
    }
}

fn trophy_tab_title(file_name: &str) -> String {
    let stem = Path::new(file_name)
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_default();
    let lower = stem.to_lowercase();
    if let Some(rest) = lower.strip_prefix("trophy").and_then(|r| r.strip_suffix(".ucp")) {
        if !rest.is_empty() && rest.chars().all(|c| c.is_ascii_digit()) {
            return format!("Trophy {rest}");
        }
    }
    Path::new(file_name)
        .file_stem()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or(stem)
}

/// Extract a trophy icon (`trop<id>.png` or zero-padded `trop%04d.png`) to
/// `cache_dir`, returning its path if found.
fn extract_icon(
    files: &HashMap<String, Vec<u8>>,
    id: &str,
    cache_dir: &Path,
) -> Option<String> {
    let mut candidates = vec![format!("trop{id}.png")];
    if let Ok(n) = id.parse::<u32>() {
        candidates.push(format!("trop{n:04}.png"));
    }

    for name in candidates {
        if let Some(bytes) = find_file(files, &name) {
            if let Err(e) = std::fs::create_dir_all(cache_dir) {
                eprintln!("trophy icon cache: could not create {}: {e}", cache_dir.display());
                return None;
            }
            let out_path = cache_dir.join(format!("{id}.png"));
            match std::fs::write(&out_path, bytes) {
                Ok(()) => return Some(out_path.to_string_lossy().to_string()),
                Err(e) => {
                    eprintln!("trophy icon cache: could not write {}: {e}", out_path.display());
                    return None;
                }
            }
        }
    }
    None
}

fn build_trophy_set(ucp_file: &Path, icon_cache_dir: &Path) -> Result<TrophySet, String> {
    let files = read_ucp(ucp_file)?;
    let file_name = ucp_file.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();

    let conf_data = find_file(&files, "tropconf.json")
        .ok_or_else(|| format!("{file_name} does not contain tropconf.json."))?;
    let tropconf: Value = serde_json::from_slice(conf_data)
        .map_err(|e| format!("Could not read tropconf.json: {e}"))?;

    let default_language =
        json_string(tropconf.get("defaultLanguage").unwrap_or(&Value::Null)).trim().to_string();
    let meta_data = (!default_language.is_empty())
        .then(|| find_file(&files, &format!("tropmeta_{default_language}.json")))
        .flatten()
        .or_else(|| find_first_trophy_metadata_file(&files))
        .ok_or_else(|| format!("{file_name} does not contain readable trophy metadata."))?;

    let tropmeta: Value = serde_json::from_slice(meta_data)
        .map_err(|e| format!("Could not read trophy metadata: {e}"))?;

    let definitions = read_definitions(&tropconf);
    if definitions.is_empty() {
        return Err(format!("{file_name} does not define any trophies."));
    }
    let texts = read_metadata(&tropmeta);

    let trophy_cache_dir = icon_cache_dir.join(file_name.trim_end_matches(".ucp"));
    let trophies = definitions
        .into_iter()
        .map(|def| {
            let text = texts.get(&def.id);
            let mut name = text.map(|t| t.name.clone()).unwrap_or_default();
            let mut detail = text.map(|t| t.detail.clone()).unwrap_or_default();
            let reward = text.map(|t| t.reward.clone()).unwrap_or_default();

            if name.is_empty() {
                name = if def.hidden {
                    "Hidden Trophy".to_string()
                } else {
                    format!("Trophy {}", def.id)
                };
            }
            if detail.is_empty() && def.hidden {
                detail = "This trophy is hidden.".to_string();
            }

            TrophyRow {
                id: def.id.clone(),
                name,
                detail,
                grade_text: grade_to_text(&def.grade),
                grade: def.grade,
                reward,
                hidden: def.hidden,
                has_reward: def.has_reward,
                icon_path: extract_icon(&files, &def.id, &trophy_cache_dir),
            }
        })
        .collect();

    Ok(TrophySet { tab_title: trophy_tab_title(&file_name), trophies })
}

/// `sce_sys/trophy2/Trophy*.ucp` (case-insensitive), deduplicated by
/// canonical path — mirrors `FindTrophyFiles`.
pub fn find_trophy_files(basedir: &str) -> Vec<PathBuf> {
    if basedir.is_empty() {
        return vec![];
    }
    let trophy_dir = Path::new(basedir).join("sce_sys/trophy2");
    let Ok(entries) = std::fs::read_dir(&trophy_dir) else {
        return vec![];
    };

    let mut seen = std::collections::HashSet::new();
    let mut files: Vec<PathBuf> = entries
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            p.is_file()
                && p.extension().map(|e| e.eq_ignore_ascii_case("ucp")).unwrap_or(false)
                && p.file_stem()
                    .map(|s| s.to_string_lossy().to_lowercase().starts_with("trophy"))
                    .unwrap_or(false)
        })
        .filter(|p| {
            let key = p.canonicalize().unwrap_or_else(|_| p.clone()).to_string_lossy().to_lowercase();
            seen.insert(key)
        })
        .collect();

    files.sort();
    files
}

pub fn has_trophy_data(basedir: &str) -> bool {
    !find_trophy_files(basedir).is_empty()
}

/// Trophy counts by grade -- how many of each grade a game's trophy pack
/// *defines*, not how many are earned (nothing in this stack tracks
/// unlocks; see load_trophies' doc and Home.tsx/Profile.tsx's trophy OSD).
/// Real, verifiable data (parsed straight from tropconf.json's own `grade`
/// field), just not the whole trophy record -- deliberately cheaper than
/// `build_trophy_set`: no tropmeta lookup, no icon extraction, so this is
/// safe to call once per game while a rail/grid selection moves.
#[derive(Debug, Clone, Copy, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TrophyCounts {
    pub platinum: u32,
    pub gold: u32,
    pub silver: u32,
    pub bronze: u32,
}

impl TrophyCounts {
    // Only the tests below sum the four grades directly -- the frontend has
    // its own trophyTotal() (store/trophies.ts) for the same count, computed
    // from the camelCase JSON this struct serializes to. cfg(test) instead
    // of #[allow(dead_code)] because that is the true and complete set of
    // callers, not a suppression.
    #[cfg(test)]
    fn total(&self) -> u32 {
        self.platinum + self.gold + self.silver + self.bronze
    }
}

fn count_definitions(tropconf: &Value, counts: &mut TrophyCounts) {
    for def in read_definitions(tropconf) {
        match def.grade.as_str() {
            "P" => counts.platinum += 1,
            "G" => counts.gold += 1,
            "S" => counts.silver += 1,
            "B" => counts.bronze += 1,
            _ => {}
        }
    }
}

pub fn count_trophies(basedir: &str) -> TrophyCounts {
    let mut counts = TrophyCounts::default();
    for file in find_trophy_files(basedir) {
        let Ok(files) = read_ucp(&file) else { continue };
        let Some(conf_data) = find_file(&files, "tropconf.json") else { continue };
        let Ok(tropconf) = serde_json::from_slice::<Value>(conf_data) else { continue };
        count_definitions(&tropconf, &mut counts);
    }
    counts
}

pub fn load_trophies(basedir: &str, icon_cache_dir: &Path) -> Result<Vec<TrophySet>, String> {
    let files = find_trophy_files(basedir);
    if files.is_empty() {
        return Err("No trophy package found in sce_sys/trophy2.".to_string());
    }

    let mut sets = Vec::new();
    let mut errors = Vec::new();
    for file in files {
        match build_trophy_set(&file, icon_cache_dir) {
            Ok(set) => sets.push(set),
            Err(e) => errors.push(e),
        }
    }

    if sets.is_empty() {
        return Err(errors.join("\n"));
    }
    Ok(sets)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_ucp(path: &Path, entries: &[(&str, &[u8])]) {
        let mut data = vec![0u8; UCP_HEADER_LEN];
        data[0..4].copy_from_slice(&UCP_MAGIC.to_be_bytes());
        data[4..8].copy_from_slice(&UCP_VERSION.to_be_bytes());
        data[0x10..0x14].copy_from_slice(&(entries.len() as u32).to_be_bytes());
        data[0x14..0x18].copy_from_slice(&(UCP_HEADER_LEN as u32).to_be_bytes());

        let toc_start = data.len();
        let table_size = UCP_TOC_SKIP + entries.len() * UCP_ENTRY_LEN;
        data.resize(toc_start + table_size, 0);

        let mut payload_offset = toc_start + table_size;
        let mut payloads = Vec::new();
        for (i, (name, bytes)) in entries.iter().enumerate() {
            let entry_offset = toc_start + UCP_TOC_SKIP + i * UCP_ENTRY_LEN;
            let name_bytes = name.as_bytes();
            data[entry_offset..entry_offset + name_bytes.len()].copy_from_slice(name_bytes);
            data[entry_offset + 0x20..entry_offset + 0x28]
                .copy_from_slice(&(payload_offset as u64).to_be_bytes());
            data[entry_offset + 0x28..entry_offset + 0x30]
                .copy_from_slice(&(bytes.len() as u64).to_be_bytes());
            payloads.push(*bytes);
            payload_offset += bytes.len();
        }
        for bytes in payloads {
            data.extend_from_slice(bytes);
        }

        let total_len = data.len() as u64;
        data[0x08..0x10].copy_from_slice(&total_len.to_be_bytes());
        std::fs::write(path, data).unwrap();
    }

    #[test]
    fn parses_a_minimal_synthetic_ucp() {
        let dir = tempfile::tempdir().unwrap();
        let tropconf = serde_json::json!({
            "defaultLanguage": "en-US",
            "trophies": [
                { "id": "0", "grade": "P", "hidden": false, "hasReward": false }
            ]
        });
        let tropmeta = serde_json::json!({
            "metadata": { "trophyMetadata": [
                { "id": "0", "name": "Platinum Trophy", "detail": "Unlock everything." }
            ]}
        });
        let conf_bytes = serde_json::to_vec(&tropconf).unwrap();
        let meta_bytes = serde_json::to_vec(&tropmeta).unwrap();

        let ucp_path = dir.path().join("Trophy00.ucp");
        write_ucp(
            &ucp_path,
            &[("tropconf.json", &conf_bytes), ("tropmeta_en-US.json", &meta_bytes)],
        );

        let icon_cache = dir.path().join("cache");
        let set = build_trophy_set(&ucp_path, &icon_cache).unwrap();
        assert_eq!(set.tab_title, "Trophy 00");
        assert_eq!(set.trophies.len(), 1);
        assert_eq!(set.trophies[0].name, "Platinum Trophy");
        assert_eq!(set.trophies[0].grade_text, "Platinum");
        assert!(set.trophies[0].icon_path.is_none());
    }

    #[test]
    fn rejects_bad_magic() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("bad.ucp");
        std::fs::write(&path, vec![0u8; UCP_HEADER_LEN]).unwrap();
        assert!(read_ucp(&path).unwrap_err().contains("invalid trophy package magic"));
    }

    #[test]
    fn find_trophy_files_matches_case_insensitively() {
        let dir = tempfile::tempdir().unwrap();
        let trophy_dir = dir.path().join("sce_sys/trophy2");
        std::fs::create_dir_all(&trophy_dir).unwrap();
        std::fs::write(trophy_dir.join("Trophy00.ucp"), b"x").unwrap();
        std::fs::write(trophy_dir.join("not_a_trophy.txt"), b"x").unwrap();

        let files = find_trophy_files(dir.path().to_str().unwrap());
        assert_eq!(files.len(), 1);
    }

    #[test]
    fn grade_letters_map_to_names() {
        assert_eq!(grade_to_text("P"), "Platinum");
        assert_eq!(grade_to_text("G"), "Gold");
        assert_eq!(grade_to_text("S"), "Silver");
        assert_eq!(grade_to_text("B"), "Bronze");
        assert_eq!(grade_to_text("?"), "?");
    }

    #[test]
    fn counts_trophies_by_grade_across_multiple_packs() {
        let dir = tempfile::tempdir().unwrap();
        let trophy_dir = dir.path().join("sce_sys/trophy2");
        std::fs::create_dir_all(&trophy_dir).unwrap();

        let conf_a = serde_json::json!({
            "trophies": [
                { "id": "0", "grade": "P", "hidden": false, "hasReward": false },
                { "id": "1", "grade": "G", "hidden": false, "hasReward": false },
                { "id": "2", "grade": "G", "hidden": false, "hasReward": false },
            ]
        });
        let conf_b = serde_json::json!({
            "trophies": [
                { "id": "0", "grade": "S", "hidden": false, "hasReward": false },
                { "id": "1", "grade": "B", "hidden": false, "hasReward": false },
                { "id": "2", "grade": "B", "hidden": false, "hasReward": false },
                { "id": "3", "grade": "B", "hidden": false, "hasReward": false },
            ]
        });

        write_ucp(&trophy_dir.join("Trophy00.ucp"), &[("tropconf.json", &serde_json::to_vec(&conf_a).unwrap())]);
        write_ucp(&trophy_dir.join("Trophy01.ucp"), &[("tropconf.json", &serde_json::to_vec(&conf_b).unwrap())]);

        let counts = count_trophies(dir.path().to_str().unwrap());
        assert_eq!(counts.platinum, 1);
        assert_eq!(counts.gold, 2);
        assert_eq!(counts.silver, 1);
        assert_eq!(counts.bronze, 3);
        assert_eq!(counts.total(), 7);
    }

    #[test]
    fn counts_trophies_empty_when_no_pack() {
        let dir = tempfile::tempdir().unwrap();
        let counts = count_trophies(dir.path().to_str().unwrap());
        assert_eq!(counts.total(), 0);
    }
}
