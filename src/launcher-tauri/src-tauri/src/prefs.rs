//! App-only preferences that have no place in the shared `Kyty.ini` (the Qt
//! launcher wouldn't understand them): emulator path override and default
//! launch mode. Stored as plain JSON in the Tauri app data dir.

use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum LaunchMode {
    InApp,
    Terminal,
}

impl Default for LaunchMode {
    fn default() -> Self {
        LaunchMode::InApp
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LauncherPrefs {
    #[serde(default)]
    pub emulator_path_override: Option<String>,
    #[serde(default)]
    pub launch_mode: LaunchMode,
    /// Emulator Settings toggle, default on: exits the launcher's own GUI
    /// process the moment a game starts (in-app launch mode only) and hands
    /// the emulator to a detached supervisor (see `supervisor.rs`) that
    /// relaunches the launcher once the game closes. On by default because
    /// it reclaims the launcher's own memory/CPU for the game while it's
    /// running, and the launcher still comes back on its own once the game
    /// exits. A user who turns it off keeps that choice on future launches.
    #[serde(default = "default_true")]
    pub auto_close_on_launch: bool,
}

fn default_true() -> bool {
    true
}

impl Default for LauncherPrefs {
    fn default() -> Self {
        LauncherPrefs {
            emulator_path_override: None,
            launch_mode: LaunchMode::default(),
            auto_close_on_launch: true,
        }
    }
}

const FILE_NAME: &str = "prefs.json";

pub fn load(app_data_dir: &Path) -> LauncherPrefs {
    std::fs::read_to_string(app_data_dir.join(FILE_NAME))
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

pub fn save(app_data_dir: &Path, prefs: &LauncherPrefs) -> std::io::Result<()> {
    std::fs::create_dir_all(app_data_dir)?;
    let text = serde_json::to_string_pretty(prefs)?;
    std::fs::write(app_data_dir.join(FILE_NAME), text)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_file_returns_defaults() {
        let dir = tempfile::tempdir().unwrap();
        let prefs = load(dir.path());
        assert_eq!(prefs.launch_mode, LaunchMode::InApp);
    }

    #[test]
    fn json_shape_is_camel_case() {
        let prefs = LauncherPrefs {
            emulator_path_override: Some("/opt/kyty_emulator".to_string()),
            launch_mode: LaunchMode::Terminal,
            auto_close_on_launch: true,
        };
        let json = serde_json::to_value(&prefs).unwrap();
        assert_eq!(json["emulatorPathOverride"], "/opt/kyty_emulator");
        assert_eq!(json["autoCloseOnLaunch"], true);
        assert!(json.get("emulator_path_override").is_none());
    }

    #[test]
    fn round_trips() {
        let dir = tempfile::tempdir().unwrap();
        let prefs = LauncherPrefs {
            emulator_path_override: Some("/opt/kyty/kyty_emulator".to_string()),
            launch_mode: LaunchMode::Terminal,
            auto_close_on_launch: true,
        };
        save(dir.path(), &prefs).unwrap();
        let reloaded = load(dir.path());
        assert_eq!(reloaded.emulator_path_override, prefs.emulator_path_override);
        assert_eq!(reloaded.launch_mode, LaunchMode::Terminal);
        assert_eq!(reloaded.auto_close_on_launch, true);
    }

    #[test]
    fn auto_close_on_launch_defaults_to_on() {
        let dir = tempfile::tempdir().unwrap();
        let prefs = load(dir.path());
        assert_eq!(prefs.auto_close_on_launch, true);
    }

    #[test]
    fn auto_close_on_launch_defaults_to_on_when_key_missing_from_json() {
        let prefs: LauncherPrefs = serde_json::from_str("{}").unwrap();
        assert_eq!(prefs.auto_close_on_launch, true);
    }
}
