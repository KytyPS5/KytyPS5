use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum PssrUpscaler {
    Linear,
    DLSS,
    FSR,
}

impl PssrUpscaler {
    pub fn as_cli_arg(self) -> &'static str {
        match self {
            PssrUpscaler::Linear => "Linear",
            PssrUpscaler::DLSS => "DLSS",
            PssrUpscaler::FSR => "FSR",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            PssrUpscaler::Linear => "Linear (Varsayılan)",
            PssrUpscaler::DLSS => "NVIDIA DLSS",
            PssrUpscaler::FSR => "AMD FSR",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct AppConfig {
    pub emulator_path: String,
    pub games_dir: String,
    pub user_name: String,
    pub screen_width: u32,
    pub screen_height: u32,
    pub fullscreen: bool,
    pub hdr: bool,
    pub vblank_frequency: u32,
    pub shader_cache: bool,
    pub perf_overlay: bool,
    pub trinity_mode: bool,
    pub pssr_upscaler: PssrUpscaler,
    pub amd_cpu: bool,
    pub native_rt: bool,
    pub emulate_psn: bool,
    pub network_connected: bool,
    pub skip_notice_screen: bool,
    pub controller_vibration: u32,
    pub controller_volume: u32,
    pub controller_color: String,
    pub console_language: u32,
    pub show_demo_games: bool,
    pub skip_boot_intro: bool,
}

impl Default for AppConfig {
    fn default() -> Self {
        let default_emu = if cfg!(target_os = "windows") {
            "kyty_emulator.exe"
        } else {
            "kyty_emulator"
        };
        Self {
            emulator_path: default_emu.to_string(),
            games_dir: String::new(),
            user_name: "Kyty".to_string(),
            screen_width: 1920,
            screen_height: 1080,
            fullscreen: false,
            hdr: false,
            vblank_frequency: 60,
            shader_cache: true,
            perf_overlay: false,
            trinity_mode: false,
            pssr_upscaler: PssrUpscaler::Linear,
            amd_cpu: false,
            native_rt: false,
            emulate_psn: false,
            network_connected: false,
            skip_notice_screen: true,
            controller_vibration: 100,
            controller_volume: 50,
            controller_color: "#0070D1".to_string(),
            console_language: 1,
            show_demo_games: true,
            skip_boot_intro: false,
        }
    }
}

impl AppConfig {
    pub fn config_path() -> PathBuf {
        if let Ok(exe) = std::env::current_exe() {
            if let Some(parent) = exe.parent() {
                return parent.join("ps5_ui_config.json");
            }
        }
        PathBuf::from("ps5_ui_config.json")
    }

    pub fn load() -> Self {
        let path = Self::config_path();
        if let Ok(content) = fs::read_to_string(&path) {
            if let Ok(cfg) = serde_json::from_str::<AppConfig>(&content) {
                return cfg;
            }
        }
        Self::default()
    }

    pub fn save(&self) -> Result<(), String> {
        let path = Self::config_path();
        let json = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        fs::write(path, json).map_err(|e| e.to_string())
    }
}
