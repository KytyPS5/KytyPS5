use egui::Color32;
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GameKind {
    Ps5Folder,
    Elf,
    ZArchive,
    Demo,
    AddGameTile,
    LibraryTile,
}

#[derive(Debug, Clone)]
pub struct GameEntry {
    pub title: String,
    pub subtitle: String,
    pub title_id: String,
    pub version: String,
    pub path: Option<PathBuf>,
    pub icon_path: Option<PathBuf>,
    pub bg_path: Option<PathBuf>,
    pub embedded_icon: Option<(&'static str, &'static [u8])>,
    pub embedded_bg: Option<(&'static str, &'static [u8])>,
    pub kind: GameKind,
    pub accent_primary: Color32,
    pub accent_secondary: Color32,
    pub badge_text: String,
}

#[derive(Debug, Clone)]
pub struct MediaEntry {
    pub title: String,
    pub subtitle: String,
    pub url: String,
    pub accent_primary: Color32,
    pub accent_secondary: Color32,
    pub icon_glyph: String,
}

pub fn default_media_entries() -> Vec<MediaEntry> {
    vec![
        MediaEntry {
            title: "YouTube".into(),
            subtitle: "4K HDR Video & Oyun Yayınları".into(),
            url: "https://www.youtube.com".into(),
            accent_primary: Color32::from_rgb(220, 38, 38),
            accent_secondary: Color32::from_rgb(80, 12, 18),
            icon_glyph: "▶".into(),
        },
        MediaEntry {
            title: "Spotify".into(),
            subtitle: "Oyun oynarken müzik dinle".into(),
            url: "https://open.spotify.com".into(),
            accent_primary: Color32::from_rgb(30, 215, 96),
            accent_secondary: Color32::from_rgb(10, 58, 32),
            icon_glyph: "♫".into(),
        },
        MediaEntry {
            title: "Twitch".into(),
            subtitle: "Canlı oyun yayınları".into(),
            url: "https://www.twitch.tv".into(),
            accent_primary: Color32::from_rgb(145, 70, 255),
            accent_secondary: Color32::from_rgb(42, 18, 86),
            icon_glyph: "◈".into(),
        },
        MediaEntry {
            title: "KytyPS5 GitHub".into(),
            subtitle: "Resmi güncellemeler ve uyumluluk raporları".into(),
            url: "https://github.com/KytyPS5/KytyPS5".into(),
            accent_primary: Color32::from_rgb(0, 112, 209),
            accent_secondary: Color32::from_rgb(10, 34, 72),
            icon_glyph: "⌘".into(),
        },
    ]
}

pub fn demo_games() -> Vec<GameEntry> {
    vec![
        GameEntry {
            title: "ASTRO's PLAYROOM".into(),
            subtitle: "Team ASOBI • DualSense Teknoloji Vitrini".into(),
            title_id: "PPSA01325".into(),
            version: "01.900.000".into(),
            path: None,
            icon_path: None,
            bg_path: None,
            embedded_icon: Some(("bytes://astro_icon.jpg", include_bytes!("../assets/covers/astro_icon.jpg"))),
            embedded_bg: Some(("bytes://astro_bg.jpg", include_bytes!("../assets/covers/astro_bg.jpg"))),
            kind: GameKind::Demo,
            accent_primary: Color32::from_rgb(0, 148, 255),
            accent_secondary: Color32::from_rgb(8, 36, 94),
            badge_text: "PS5".into(),
        },
        GameEntry {
            title: "Demon's Souls".into(),
            subtitle: "Bluepoint Games • Karanlık Fantezi Aksiyon RPG".into(),
            title_id: "PPSA01342".into(),
            version: "01.004.000".into(),
            path: None,
            icon_path: None,
            bg_path: None,
            embedded_icon: Some(("bytes://demons_icon.jpg", include_bytes!("../assets/covers/demons_icon.jpg"))),
            embedded_bg: Some(("bytes://demons_bg.jpg", include_bytes!("../assets/covers/demons_bg.jpg"))),
            kind: GameKind::Demo,
            accent_primary: Color32::from_rgb(206, 154, 72),
            accent_secondary: Color32::from_rgb(38, 28, 20),
            badge_text: "PS5".into(),
        },
        GameEntry {
            title: "Ratchet & Clank: Rift Apart".into(),
            subtitle: "Insomniac Games • Boyutlar Arası Macera".into(),
            title_id: "PPSA01473".into(),
            version: "01.005.000".into(),
            path: None,
            icon_path: None,
            bg_path: None,
            embedded_icon: Some(("bytes://ratchet_icon.jpg", include_bytes!("../assets/covers/ratchet_icon.jpg"))),
            embedded_bg: Some(("bytes://ratchet_bg.jpg", include_bytes!("../assets/covers/ratchet_bg.jpg"))),
            kind: GameKind::Demo,
            accent_primary: Color32::from_rgb(176, 72, 245),
            accent_secondary: Color32::from_rgb(36, 16, 78),
            badge_text: "PS5".into(),
        },
        GameEntry {
            title: "Returnal".into(),
            subtitle: "Housemarque • Bilim Kurgu Roguelike".into(),
            title_id: "PPSA01284".into(),
            version: "03.000.000".into(),
            path: None,
            icon_path: None,
            bg_path: None,
            embedded_icon: Some(("bytes://returnal_icon.jpg", include_bytes!("../assets/covers/returnal_icon.jpg"))),
            embedded_bg: Some(("bytes://returnal_bg.jpg", include_bytes!("../assets/covers/returnal_bg.jpg"))),
            kind: GameKind::Demo,
            accent_primary: Color32::from_rgb(44, 198, 178),
            accent_secondary: Color32::from_rgb(12, 42, 48),
            badge_text: "PS5".into(),
        },
        GameEntry {
            title: "Gran Turismo 7".into(),
            subtitle: "Polyphony Digital • Gerçek Sürüş Simülatörü".into(),
            title_id: "PPSA01317".into(),
            version: "01.490.000".into(),
            path: None,
            icon_path: None,
            bg_path: None,
            embedded_icon: Some(("bytes://gt7_icon.jpg", include_bytes!("../assets/covers/gt7_icon.jpg"))),
            embedded_bg: Some(("bytes://gt7_bg.jpg", include_bytes!("../assets/covers/gt7_bg.jpg"))),
            kind: GameKind::Demo,
            accent_primary: Color32::from_rgb(230, 56, 72),
            accent_secondary: Color32::from_rgb(42, 16, 26),
            badge_text: "PS5".into(),
        },
    ]
}

pub fn build_game_list(games_dir: &str, include_demo: bool) -> Vec<GameEntry> {
    let mut entries = Vec::new();

    if !games_dir.trim().is_empty() {
        let root = Path::new(games_dir.trim());
        if root.is_dir() {
            // Check if the root itself is a single PS5 game directory
            if let Some(single) = inspect_game_dir(root) {
                entries.push(single);
            } else if let Ok(read_dir) = fs::read_dir(root) {
                for entry in read_dir.flatten() {
                    let path = entry.path();
                    if path.is_dir() {
                        if let Some(game) = inspect_game_dir(&path) {
                            entries.push(game);
                        }
                    } else if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
                        let ext_lower = ext.to_lowercase();
                        if ext_lower == "elf" || ext_lower == "zar" {
                            let name = path
                                .file_stem()
                                .and_then(|s| s.to_str())
                                .unwrap_or("Bilinmeyen Oyun")
                                .to_string();
                            let is_zar = ext_lower == "zar";
                            entries.push(GameEntry {
                                title: name,
                                subtitle: path.display().to_string(),
                                title_id: if is_zar { "ZARCHIVE" } else { "HOMEBREW" }.into(),
                                version: "1.00".into(),
                                path: Some(path),
                                icon_path: None,
                                bg_path: None,
                                embedded_icon: None,
                                embedded_bg: None,
                                kind: if is_zar {
                                    GameKind::ZArchive
                                } else {
                                    GameKind::Elf
                                },
                                accent_primary: if is_zar {
                                    Color32::from_rgb(56, 152, 255)
                                } else {
                                    Color32::from_rgb(64, 212, 148)
                                },
                                accent_secondary: Color32::from_rgb(16, 30, 56),
                                badge_text: if is_zar { "PS5 • ZAR" } else { "PS5 • ELF" }.into(),
                            });
                        }
                    }
                }
            }
        }
    }

    entries.sort_by(|a, b| a.title.to_lowercase().cmp(&b.title.to_lowercase()));

    if include_demo && entries.is_empty() {
        entries.extend(demo_games());
    }

    // Utility tiles at the end of the ribbon (authentic PS5 feel)
    entries.push(GameEntry {
        title: "Oyun veya Klasör Ekle".into(),
        subtitle: "Bilgisayarından PS5 oyun klasörü, .elf veya .zar dosyası seç".into(),
        title_id: "KYTYPS5".into(),
        version: "".into(),
        path: None,
        icon_path: None,
        bg_path: None,
        embedded_icon: None,
        embedded_bg: None,
        kind: GameKind::AddGameTile,
        accent_primary: Color32::from_rgb(0, 112, 209),
        accent_secondary: Color32::from_rgb(14, 24, 46),
        badge_text: "+ EKLE".into(),
    });

    entries.push(GameEntry {
        title: "Oyun Kütüphanesi".into(),
        subtitle: "Oyun klasörünü yeniden tara veya ayarları aç".into(),
        title_id: "LIBRARY".into(),
        version: "".into(),
        path: None,
        icon_path: None,
        bg_path: None,
        embedded_icon: None,
        embedded_bg: None,
        kind: GameKind::LibraryTile,
        accent_primary: Color32::from_rgb(96, 118, 168),
        accent_secondary: Color32::from_rgb(18, 22, 36),
        badge_text: "KÜTÜPHANE".into(),
    });

    entries
}

pub fn inspect_game_dir(dir: &Path) -> Option<GameEntry> {
    let eboot = dir.join("eboot.bin");
    let param_json = dir.join("sce_sys").join("param.json");

    if !eboot.exists() && !param_json.exists() {
        return None;
    }

    let fallback_name = dir
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("PS5 Oyunu")
        .to_string();

    let (title, title_id, version) = if param_json.exists() {
        parse_param_json(&param_json, &fallback_name)
    } else {
        (fallback_name, "PS5-DIR".into(), "1.00".into())
    };

    let icon_path = ["sce_sys/icon0.png", "icon0.png"]
        .iter()
        .map(|rel| dir.join(rel))
        .find(|p| p.exists());

    let bg_path = ["sce_sys/pic1.png", "sce_sys/pic0.png", "pic1.png", "pic0.png"]
        .iter()
        .map(|rel| dir.join(rel))
        .find(|p| p.exists());

    // Deterministic accent tint from title hash
    let hash: u32 = title
        .bytes()
        .fold(2166136261u32, |acc, b| acc.wrapping_mul(16777619) ^ b as u32);
    let palette = [
        (Color32::from_rgb(0, 140, 255), Color32::from_rgb(10, 32, 74)),
        (Color32::from_rgb(156, 86, 255), Color32::from_rgb(32, 16, 68)),
        (Color32::from_rgb(236, 98, 86), Color32::from_rgb(56, 18, 26)),
        (Color32::from_rgb(42, 198, 168), Color32::from_rgb(12, 42, 46)),
        (Color32::from_rgb(218, 166, 68), Color32::from_rgb(44, 30, 14)),
    ];
    let (accent_primary, accent_secondary) = palette[(hash as usize) % palette.len()];

    Some(GameEntry {
        title,
        subtitle: dir.display().to_string(),
        title_id,
        version,
        path: Some(dir.to_path_buf()),
        icon_path,
        bg_path,
        embedded_icon: None,
        embedded_bg: None,
        kind: GameKind::Ps5Folder,
        accent_primary,
        accent_secondary,
        badge_text: "PS5".into(),
    })
}

fn parse_param_json(path: &Path, fallback_name: &str) -> (String, String, String) {
    let Ok(raw) = fs::read_to_string(path) else {
        return (fallback_name.to_string(), "PS5".into(), "1.00".into());
    };
    let Ok(v) = serde_json::from_str::<Value>(&raw) else {
        return (fallback_name.to_string(), "PS5".into(), "1.00".into());
    };

    let title_id = v
        .get("titleId")
        .and_then(|x| x.as_str())
        .unwrap_or("PS5")
        .to_string();

    let version = v
        .get("contentVersion")
        .and_then(|x| x.as_str())
        .unwrap_or("01.000.000")
        .to_string();

    let mut title = None;
    if let Some(loc) = v.get("localizedParameters") {
        let default_lang = loc
            .get("defaultLanguage")
            .and_then(|x| x.as_str())
            .unwrap_or("en-US");
        title = loc
            .get("tr-TR")
            .and_then(|l| l.get("titleName"))
            .and_then(|t| t.as_str())
            .or_else(|| {
                loc.get(default_lang)
                    .and_then(|l| l.get("titleName"))
                    .and_then(|t| t.as_str())
            })
            .or_else(|| {
                loc.get("en-US")
                    .and_then(|l| l.get("titleName"))
                    .and_then(|t| t.as_str())
            })
            .map(|s| s.to_string());
    }

    let final_title = title.unwrap_or_else(|| fallback_name.to_string());
    (final_title, title_id, version)
}
