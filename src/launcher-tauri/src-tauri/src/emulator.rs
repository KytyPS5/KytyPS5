//! Emulator discovery, argv construction, and process launching — a Rust
//! port of `mainDialog.cpp`'s `FindInterpreter`, `CreateEmulatorArgs`,
//! `CreateBashScript`, `FindTerminal`, and `RunInterpreter`.

use crate::config::Configuration;
use serde::Serialize;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use tauri::{AppHandle, Emitter};

#[cfg(not(windows))]
const EMULATOR_EXE: &str = "kyty_emulator";
#[cfg(windows)]
const EMULATOR_EXE: &str = "kyty_emulator.exe";

#[cfg(unix)]
const KYTY_BASH_FILE: &str = "kyty_run.sh";

/// Terminal candidates and their argument separator, in the same priority
/// order as `FindTerminal` in mainDialog.cpp. `None` means the command
/// follows immediately with no separator token (kitty/foot).
#[cfg(unix)]
const TERMINAL_CANDIDATES: &[(&str, Option<&str>)] = &[
    ("x-terminal-emulator", Some("-e")),
    ("gnome-terminal", Some("--")),
    ("konsole", Some("-e")),
    ("xfce4-terminal", Some("-x")),
    ("mate-terminal", Some("--")),
    ("tilix", Some("-e")),
    ("alacritty", Some("-e")),
    ("kitty", None),
    ("foot", None),
    ("wezterm", Some("-e")),
    ("urxvt", Some("-e")),
    ("xterm", Some("-e")),
];

fn find_executable(name: &str) -> Option<PathBuf> {
    if name.contains('/') || name.contains('\\') {
        let p = PathBuf::from(name);
        return p.is_file().then_some(p);
    }
    let path_var = std::env::var_os("PATH")?;
    std::env::split_paths(&path_var).find_map(|dir| {
        let candidate = dir.join(name);
        candidate.is_file().then_some(candidate)
    })
}

/// Locate a terminal emulator to run `kyty_run.sh` in, honoring `$TERMINAL`
/// first (matching its own separator convention when it's a known
/// candidate, else defaulting to `-e`), then walking the candidate list.
#[cfg(unix)]
pub fn find_terminal() -> Option<(PathBuf, Vec<String>)> {
    if let Ok(from_env) = std::env::var("TERMINAL") {
        if !from_env.is_empty() {
            if let Some(resolved) = find_executable(&from_env) {
                let name = Path::new(&from_env)
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or(from_env.clone());
                let separator = TERMINAL_CANDIDATES
                    .iter()
                    .find(|(exe, _)| *exe == name)
                    .map(|(_, sep)| *sep)
                    .unwrap_or(Some("-e"));
                let prefix = separator.map(|s| vec![s.to_string()]).unwrap_or_default();
                return Some((resolved, prefix));
            }
        }
    }

    for (exe, separator) in TERMINAL_CANDIDATES {
        if let Some(resolved) = find_executable(exe) {
            let prefix = separator.map(|s| vec![s.to_string()]).unwrap_or_default();
            return Some((resolved, prefix));
        }
    }
    None
}

/// The per-OS half of the dev build layout: CMake configures into
/// `_Build/<os>/`, so the `install/` tree to probe for is named after the
/// platform this launcher was built for, not always `linux`.
#[cfg(target_os = "linux")]
const BUILD_INSTALL_DIR: &str = "_Build/linux/install";
#[cfg(windows)]
const BUILD_INSTALL_DIR: &str = "_Build/windows/install";
#[cfg(target_os = "macos")]
const BUILD_INSTALL_DIR: &str = "_Build/macos/install";

/// Find `kyty_emulator` next to the app binary, its parent, walking up to a
/// `_Build/<os>/install/kyty_emulator` (the local dev build layout), or on
/// `$PATH`.
pub fn discover_emulator(app_binary_dir: &Path) -> Option<PathBuf> {
    let candidate = app_binary_dir.join(EMULATOR_EXE);
    if candidate.is_file() {
        return Some(candidate);
    }
    if let Some(parent) = app_binary_dir.parent() {
        let candidate = parent.join(EMULATOR_EXE);
        if candidate.is_file() {
            return Some(candidate);
        }
    }

    let mut dir = app_binary_dir.to_path_buf();
    for _ in 0..8 {
        let candidate = dir.join(BUILD_INSTALL_DIR).join(EMULATOR_EXE);
        if candidate.is_file() {
            return Some(candidate);
        }
        if !dir.pop() {
            break;
        }
    }

    find_executable(EMULATOR_EXE)
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorInfo {
    pub path: String,
    pub version: String,
}

/// Run the emulator binary with no arguments and read its self-identifying
/// output, mirroring `FindInterpreter`: the first line is skipped when it
/// starts with `exe_name`, and the version line after it is used.
pub fn probe_version(binary: &Path) -> Option<String> {
    let output = Command::new(binary).output().ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    let lines: Vec<&str> = text.lines().filter(|l| !l.trim().is_empty()).collect();
    if lines.len() < 2 {
        return None;
    }
    let version = if lines[0].starts_with("exe_name") { lines[1] } else { lines[0] };
    Some(version.to_string())
}

fn bool_arg(value: bool) -> &'static str {
    if value { "true" } else { "false" }
}

/// Build the emulator argv exactly as `CreateEmulatorArgs` does.
pub fn build_args(
    info: &Configuration,
    patch_plan: Option<&Path>,
    gamepad_keymap: &[String],
    gamepad_deadzone: f64,
) -> Vec<String> {
    let (width, height) = info.screen_resolution.dimensions();
    let mut args = vec![
        "--screen-width".to_string(),
        width.to_string(),
        "--screen-height".to_string(),
        height.to_string(),
    ];
    if info.fullscreen_enabled {
        args.push("--fullscreen".to_string());
    }
    args.push("--vblank-frequency".to_string());
    args.push(info.vblank_frequency.to_string());
    args.push("--console-language".to_string());
    args.push(info.console_language.to_string());
    args.push("--vulkan-validation".to_string());
    args.push(bool_arg(info.vulkan_validation_enabled).to_string());
    args.push("--shader-validation".to_string());
    args.push(bool_arg(info.shader_validation_enabled).to_string());
    args.push("--shader-optimization-type".to_string());
    args.push(shader_optimization_cli_text(info.shader_optimization_type).to_string());
    args.push("--shader-log-direction".to_string());
    args.push(log_direction_cli_text(info.shader_log_direction).to_string());
    args.push("--shader-log-folder".to_string());
    args.push(info.shader_log_folder.clone());
    args.push("--command-buffer-dump".to_string());
    args.push(bool_arg(info.command_buffer_dump_enabled).to_string());
    args.push("--command-buffer-dump-folder".to_string());
    args.push(info.command_buffer_dump_folder.clone());
    args.push("--printf-direction".to_string());
    args.push(log_direction_cli_text(info.printf_direction).to_string());
    args.push("--printf-output-file".to_string());
    args.push(info.printf_output_file.clone());
    args.push("--profiler-direction".to_string());
    args.push(profiler_direction_cli_text(info.profiler_direction).to_string());
    args.push("--spirv-debug-printf".to_string());
    args.push("false".to_string());

    for binding in &info.host_input_mapping {
        args.push("--keymap".to_string());
        args.push(binding.clone());
    }
    for binding in gamepad_keymap {
        args.push("--gamepad-map".to_string());
        args.push(binding.clone());
    }
    if gamepad_deadzone > 0.0 {
        args.push("--gamepad-deadzone".to_string());
        args.push(gamepad_deadzone.to_string());
    }
    if info.renderdoc_enabled {
        args.push("--rd".to_string());
    }
    if info.bvh_stub_enabled {
        args.push("--stub-bvh".to_string());
    }

    let game = if info.elf.is_empty() {
        info.basedir.clone()
    } else {
        Path::new(&info.basedir).join(&info.elf).to_string_lossy().to_string()
    };
    args.push("--game".to_string());
    args.push(game);

    if let Some(plan) = patch_plan {
        if plan.is_file() {
            args.push("--game-patch".to_string());
            args.push(plan.to_string_lossy().to_string());
        }
    }

    args
}

// CLI-friendly enum spellings. kyty_emulator's own --help is explicit that
// these three flags take capitalized values (None/Size/Performance,
// Silent/Console/File, None/Network) -- these previously emitted lowercase,
// which the emulator's arg parser rejects outright ("invalid shader
// optimization type: performance"), aborting straight to --help instead of
// launching. Confirmed against the emulator's real stderr output, not
// guessed. Unlike resolution (whose CLI text is genuinely lowercased by
// EnumToText in configuration.h), these enums round-trip through the exact
// same spelling the --help text documents.
fn shader_optimization_cli_text(v: crate::config::ShaderOptimizationType) -> &'static str {
    use crate::config::ShaderOptimizationType::*;
    match v {
        None_ => "None",
        Size => "Size",
        Performance => "Performance",
    }
}

fn log_direction_cli_text(v: crate::config::LogDirection) -> &'static str {
    use crate::config::LogDirection::*;
    match v {
        Silent => "Silent",
        Console => "Console",
        File => "File",
    }
}

fn profiler_direction_cli_text(v: crate::config::ProfilerDirection) -> &'static str {
    use crate::config::ProfilerDirection::*;
    match v {
        None_ => "None",
        Network => "Network",
    }
}

#[cfg(unix)]
fn bash_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

/// Write `kyty_run.sh` next to the emulator binary, mirroring
/// `CreateBashScript`.
#[cfg(unix)]
pub fn write_bash_script(
    interpreter: &Path,
    args: &[String],
    script_path: &Path,
) -> std::io::Result<()> {
    let mut file = std::fs::File::create(script_path)?;
    writeln!(file, "#!/bin/bash")?;
    write!(file, "{}", bash_quote(&interpreter.to_string_lossy()))?;
    for arg in args {
        write!(file, " {}", bash_quote(arg))?;
    }
    writeln!(file)?;
    writeln!(file, "echo Press any key...")?;
    writeln!(file, "read -n1")?;

    let mut perms = file.metadata()?.permissions();
    use std::os::unix::fs::PermissionsExt;
    perms.set_mode(perms.mode() | 0o111);
    std::fs::set_permissions(script_path, perms)?;
    Ok(())
}

/// Launch the emulator in a separate terminal window: on Linux/macOS this
/// writes `kyty_run.sh` and opens it in whatever terminal `find_terminal`
/// locates (mirroring `CreateBashScript` + `FindTerminal`); on Windows this
/// opens `cmd.exe /K` directly with a new console, mirroring
/// `BuildWinCmdKCommand` in mainDialog.cpp — no intermediate script file,
/// since `cmd /K` takes the whole command line itself.
#[cfg(unix)]
pub fn launch_external_terminal(
    interpreter: &Path,
    args: &[String],
    working_dir: &Path,
) -> std::io::Result<()> {
    let script_path = working_dir.join(KYTY_BASH_FILE);
    write_bash_script(interpreter, args, &script_path)?;

    let mut cmd = if let Some((terminal, mut prefix)) = find_terminal() {
        let mut c = Command::new(terminal);
        prefix.push("bash".to_string());
        c.args(prefix);
        c.arg(&script_path);
        c
    } else {
        let mut c = Command::new("bash");
        c.arg(&script_path);
        c
    };
    cmd.current_dir(working_dir).spawn()?;
    Ok(())
}

#[cfg(windows)]
fn win_cmd_quote(value: &str) -> String {
    format!("\"{}\"", value.replace('"', "\\\""))
}

#[cfg(windows)]
pub fn launch_external_terminal(
    interpreter: &Path,
    args: &[String],
    working_dir: &Path,
) -> std::io::Result<()> {
    use std::os::windows::process::CommandExt;
    const CREATE_NEW_CONSOLE: u32 = 0x0000_0010;

    let mut command_line = win_cmd_quote(&interpreter.to_string_lossy());
    for arg in args {
        command_line.push(' ');
        command_line.push_str(&win_cmd_quote(arg));
    }

    Command::new("cmd.exe")
        .raw_arg(format!("/K \"{command_line}\""))
        .current_dir(working_dir)
        .creation_flags(CREATE_NEW_CONSOLE)
        .spawn()?;
    Ok(())
}

/// `kyty_emulator` is a console subsystem program, so launching it from a
/// windowed process makes Windows allocate a console window for it. Every
/// line it prints is already captured -- streamed to the in-app console and
/// teed to the session log, or redirected wholesale to the session log
/// under the supervisor -- so that window shows nothing the launcher has
/// not already got, and for anyone just playing a game it is pure noise.
///
/// There is no setting for it: a user who wants a live terminal picks
/// "External terminal" as the launch mode, which is what that mode is for.
/// No effect off Windows, where a GUI-launched child inherits no terminal
/// to begin with.
pub fn hide_console(command: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        command.creation_flags(CREATE_NO_WINDOW);
    }
    #[cfg(not(windows))]
    {
        let _ = command;
    }
}

pub struct RunState {
    pub child: Mutex<Option<Child>>,
}

impl Default for RunState {
    fn default() -> Self {
        RunState { child: Mutex::new(None) }
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LogLine {
    stream: &'static str,
    line: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct ExitInfo {
    code: Option<i32>,
}

/// Spawn the emulator directly and stream stdout/stderr to the frontend as
/// `emulator-log` events. Also starts the background exit-watcher
/// (`try_wait_loop`) so `emulator-exited` fires without the caller having to
/// remember to start it separately.
pub fn spawn_in_app(
    app: AppHandle,
    state: std::sync::Arc<RunState>,
    interpreter: &Path,
    args: &[String],
    working_dir: &Path,
    session_log: Option<crate::logs::SessionLog>,
) -> std::io::Result<()> {
    let mut command = Command::new(interpreter);
    command
        .args(args)
        .current_dir(working_dir)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    hide_console(&mut command);
    let mut child = command.spawn()?;

    // Each reader tees to two places: the in-app console, which is live but
    // lost when the app closes, and the session file, which is what a user
    // can actually attach to a bug report.
    if let Some(stdout) = child.stdout.take() {
        let app = app.clone();
        let log = session_log.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                if let Some(log) = log.as_ref() {
                    log.write_line("stdout", &line);
                }
                let _ = app.emit("emulator-log", LogLine { stream: "stdout", line });
            }
        });
    }
    if let Some(stderr) = child.stderr.take() {
        let app = app.clone();
        let log = session_log.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                if let Some(log) = log.as_ref() {
                    log.write_line("stderr", &line);
                }
                let _ = app.emit("emulator-log", LogLine { stream: "stderr", line });
            }
        });
    }

    *state.child.lock().unwrap() = Some(child);

    let watcher_state = state.clone();
    std::thread::spawn(move || try_wait_loop(app, watcher_state));

    Ok(())
}

/// Poll the running child for completion without blocking a Tauri command;
/// call this from a dedicated background thread started right after
/// `spawn_in_app`. Emits `emulator-exited` exactly once.
pub fn try_wait_loop(app: AppHandle, state: std::sync::Arc<RunState>) {
    loop {
        std::thread::sleep(std::time::Duration::from_millis(300));
        let mut guard = state.child.lock().unwrap();
        let Some(child) = guard.as_mut() else { break };
        match child.try_wait() {
            Ok(Some(status)) => {
                let _ = app.emit("emulator-exited", ExitInfo { code: status.code() });
                *guard = None;
                break;
            }
            Ok(None) => {}
            Err(_) => break,
        }
    }
}

pub fn stop(state: &RunState) -> std::io::Result<()> {
    let mut guard = state.child.lock().unwrap();
    if let Some(child) = guard.as_mut() {
        child.kill()?;
    }
    *guard = None;
    Ok(())
}

pub fn is_running(state: &RunState) -> bool {
    state.child.lock().unwrap().is_some()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::Configuration;

    #[test]
    fn build_args_matches_qt_ordering_and_spelling() {
        let mut info = Configuration::default();
        info.basedir = "/games/Astro".to_string();
        info.elf = "eboot.bin".to_string();
        info.host_input_mapping = vec!["Cross=J".to_string(), "Circle=L".to_string()];

        let args = build_args(&info, None, &[], 0.0);
        // `--game` is built with `Path::join`, so its separator is the host's
        // (`\` on Windows). That is the correct thing to hand the emulator on
        // each platform; only this literal expectation is POSIX-shaped, so
        // normalize rather than assert one platform's spelling everywhere.
        let expected_game = Path::new("/games/Astro").join("eboot.bin").to_string_lossy().to_string();
        assert_eq!(
            args,
            vec![
                "--screen-width", "1280",
                "--screen-height", "720",
                "--vblank-frequency", "60",
                "--console-language", "1",
                "--vulkan-validation", "false",
                "--shader-validation", "true",
                "--shader-optimization-type", "Performance",
                "--shader-log-direction", "Silent",
                "--shader-log-folder", "_Shaders",
                "--command-buffer-dump", "false",
                "--command-buffer-dump-folder", "_Buffers",
                "--printf-direction", "Silent",
                "--printf-output-file", "_kyty.txt",
                "--profiler-direction", "None",
                "--spirv-debug-printf", "false",
                "--keymap", "Cross=J",
                "--keymap", "Circle=L",
                "--game", expected_game.as_str(),
            ]
        );
    }

    #[test]
    fn fullscreen_and_renderdoc_flags_are_bare() {
        let mut info = Configuration::default();
        info.fullscreen_enabled = true;
        info.renderdoc_enabled = true;
        let args = build_args(&info, None, &[], 0.0);
        assert!(args.contains(&"--fullscreen".to_string()));
        assert!(args.contains(&"--rd".to_string()));
    }

    #[test]
    fn empty_elf_uses_basedir_as_game_path() {
        let mut info = Configuration::default();
        info.basedir = "/games/Astro".to_string();
        info.elf = String::new();
        let args = build_args(&info, None, &[], 0.0);
        let idx = args.iter().position(|a| a == "--game").unwrap();
        assert_eq!(args[idx + 1], "/games/Astro");
    }

    #[test]
    fn gamepad_map_entries_and_deadzone_become_cli_args() {
        let info = Configuration::default();
        let args = build_args(
            &info,
            None,
            &["Cross=a".to_string(), "LeftStickX=leftx".to_string()],
            0.2,
        );
        let idx = args.iter().position(|a| a == "--gamepad-map").unwrap();
        assert_eq!(args[idx + 1], "Cross=a");
        let second_idx =
            args.iter().enumerate().skip(idx + 1).find(|(_, a)| *a == "--gamepad-map").unwrap().0;
        assert_eq!(args[second_idx + 1], "LeftStickX=leftx");

        let dz_idx = args.iter().position(|a| a == "--gamepad-deadzone").unwrap();
        assert_eq!(args[dz_idx + 1], "0.2");
    }

    #[test]
    fn zero_deadzone_and_empty_gamepad_map_add_no_args() {
        let info = Configuration::default();
        let args = build_args(&info, None, &[], 0.0);
        assert!(!args.iter().any(|a| a == "--gamepad-map" || a == "--gamepad-deadzone"));
    }

    #[test]
    #[cfg(unix)]
    fn bash_quote_escapes_single_quotes() {
        assert_eq!(bash_quote("it's a test"), r"'it'\''s a test'");
    }

    #[test]
    #[cfg(windows)]
    fn win_cmd_quote_escapes_double_quotes() {
        assert_eq!(win_cmd_quote(r#"a "quoted" path"#), r#""a \"quoted\" path""#);
    }
}
