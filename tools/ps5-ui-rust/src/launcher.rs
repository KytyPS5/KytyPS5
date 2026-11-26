use crate::config::AppConfig;
use std::path::Path;
use std::process::{Child, Command};

pub struct RunningGame {
    pub title: String,
    pub child: Child,
}

pub fn build_command_args(cfg: &AppConfig, game_path: &Path) -> Vec<String> {
    let mut args = vec![
        "--game".to_string(),
        game_path.to_string_lossy().to_string(),
        "--screen-width".to_string(),
        cfg.screen_width.to_string(),
        "--screen-height".to_string(),
        cfg.screen_height.to_string(),
        "--vblank-frequency".to_string(),
        cfg.vblank_frequency.to_string(),
        "--controller-vibration".to_string(),
        cfg.controller_vibration.to_string(),
        "--controller-volume".to_string(),
        cfg.controller_volume.to_string(),
        "--console-language".to_string(),
        cfg.console_language.to_string(),
    ];

    if !cfg.user_name.trim().is_empty() {
        args.push("--user-name".to_string());
        args.push(cfg.user_name.trim().chars().take(16).collect());
    }

    if !cfg.controller_color.trim().is_empty() {
        args.push("--controller-color".to_string());
        args.push(cfg.controller_color.trim().to_string());
    }

    if cfg.fullscreen {
        args.push("--fullscreen".to_string());
    }
    if cfg.hdr {
        args.push("--hdr".to_string());
    }
    if cfg.shader_cache {
        args.push("--shader-cache".to_string());
    }
    if cfg.perf_overlay {
        args.push("--perf-overlay".to_string());
    }
    if cfg.trinity_mode {
        args.push("--trinity".to_string());
        args.push("--pssr-upscaler".to_string());
        args.push(cfg.pssr_upscaler.as_cli_arg().to_string());
    }
    if cfg.amd_cpu {
        args.push("--amd-cpu".to_string());
    }
    if cfg.native_rt {
        args.push("--native-rt".to_string());
    }
    if cfg.emulate_psn {
        args.push("--emulate-psn".to_string());
    }
    if cfg.network_connected {
        args.push("--network-connected".to_string());
    }
    if cfg.skip_notice_screen {
        args.push("--skip-notice-screen".to_string());
    }

    args
}

pub fn launch_game(cfg: &AppConfig, title: &str, game_path: &Path) -> Result<RunningGame, String> {
    let emu_path = Path::new(cfg.emulator_path.trim());
    if !emu_path.exists() {
        return Err(format!(
            "kyty_emulator.exe bulunamadı: {}",
            emu_path.display()
        ));
    }
    if !game_path.exists() {
        return Err(format!("Oyun yolu bulunamadı: {}", game_path.display()));
    }

    let args = build_command_args(cfg, game_path);
    let mut cmd = Command::new(emu_path);
    cmd.args(&args);
    if let Some(parent) = emu_path.parent() {
        cmd.current_dir(parent);
    }

    let child = cmd
        .spawn()
        .map_err(|e| format!("Emülatör başlatılamadı: {e}"))?;

    Ok(RunningGame {
        title: title.to_string(),
        child,
    })
}

pub fn open_url_or_path(target: &str) -> Result<(), String> {
    #[cfg(target_os = "windows")]
    {
        Command::new("cmd")
            .args(["/C", "start", "", target])
            .spawn()
            .map_err(|e| format!("Açılamadı: {e}"))?;
        Ok(())
    }
    #[cfg(not(target_os = "windows"))]
    {
        Command::new("xdg-open")
            .arg(target)
            .spawn()
            .map_err(|e| format!("Açılamadı: {e}"))?;
        Ok(())
    }
}
