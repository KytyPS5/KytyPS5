mod art;
mod audio;
mod bluetooth;
mod browse;
mod compatibility;
mod config;
mod emulator;
mod first_seen;
mod logs;
mod gamepad;
mod patches;
mod playtime;
mod prefs;
mod qsettings;
mod savedata;
mod scanner;
pub mod supervisor;
mod trophy;

#[cfg(target_os = "linux")]
mod webkit_tuning;

use compatibility::CompatibilityMap;
use config::{Configuration, KytyConfig};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use tauri::{Manager, State};

struct AppState {
    run_state: Arc<emulator::RunState>,
    compatibility: Mutex<CompatibilityMap>,
    compatibility_is_local: bool,
    /// `None` when gilrs failed to initialize (e.g. no udev on this system) --
    /// gamepad polling then just reports zero connected devices rather than
    /// panicking or breaking anything else. See poll_gamepad_state's doc
    /// comment for why this exists instead of the frontend's own Gamepad
    /// API polling.
    gilrs: Option<Mutex<gilrs::Gilrs>>,
}

/// The directory game processes and helper files (kyty_run.sh, the
/// compatibility cache, save-data roots) resolve relative to: next to the
/// discovered emulator binary, exactly like the Qt launcher's
/// `QApplication::applicationDirPath()`-relative behavior.
fn working_dir(app: &tauri::AppHandle, state: &AppState) -> PathBuf {
    emulator_binary(app, state)
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| std::env::current_dir().unwrap_or_default())
}

fn emulator_binary(app: &tauri::AppHandle, state: &AppState) -> Result<PathBuf, String> {
    let _ = state;
    if let Ok(exe_dir) = std::env::current_exe() {
        let exe_dir = exe_dir.parent().unwrap_or(Path::new(".")).to_path_buf();
        let prefs = prefs::load(&app_data_dir(app));
        if let Some(over) = prefs.emulator_path_override {
            let p = PathBuf::from(over);
            if p.is_file() {
                return Ok(p);
            }
        }
        if let Some(found) = emulator::discover_emulator(&exe_dir) {
            return Ok(found);
        }
    }
    Err("Could not find kyty_emulator".to_string())
}

/// Where this launcher keeps its own files: prefs, play history, trophy and
/// art caches, and the session logs.
///
/// Deliberately not Tauri's `app_data_dir()`, which names the folder after
/// the bundle identifier -- `io.github.kytyps5.launcher`. That is the right
/// convention for something nobody opens by hand, but the Console page now
/// tells people to go and fetch a log out of it, and Kyty already has an
/// obvious home: `Kyty.ini` lives in `<config>/Kyty/`, so this sits beside
/// it as `<config>/Kyty/Launcher/`.
///
/// Anything already written under the old identifier folder is moved across
/// the first time this runs, so upgrading keeps play counts and settings.
fn app_data_dir(app: &tauri::AppHandle) -> PathBuf {
    let dir = dirs::config_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("Kyty")
        .join("Launcher");

    if !dir.exists() {
        if let Ok(legacy) = app.path().app_data_dir() {
            if legacy.is_dir() {
                if let Some(parent) = dir.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                // A rename across the same volume is atomic and cheap. If it
                // fails (different volume, file in use), fall through and
                // just start fresh in the new location rather than failing.
                let _ = std::fs::rename(&legacy, &dir);
            }
        }
    }
    dir
}

// ---- Settings (Kyty.ini) --------------------------------------------------

#[tauri::command]
fn load_config() -> KytyConfig {
    config::load(&config::resolve_settings_path())
}

#[tauri::command]
fn save_config(cfg: KytyConfig) -> Result<(), String> {
    config::save(&config::resolve_settings_path(), &cfg).map_err(|e| e.to_string())
}

// ---- Game scanning ---------------------------------------------------------

/// Rescans every configured game folder and merges each hit against the
/// current `Kyty.ini` (custom per-game overrides, else global defaults) —
/// the Rust equivalent of `ConfigurationListWidget::ScanGameDirectory`.
/// Always reads game folders fresh off disk rather than trusting a
/// frontend-supplied list, so a settings change made in the Qt launcher is
/// picked up too.
#[tauri::command]
fn scan_games(app: tauri::AppHandle) -> Vec<scanner::GameEntry> {
    let cfg = config::load(&config::resolve_settings_path());
    let scanned = scanner::scan_game_directories(&cfg.game_dirs);
    let paths: Vec<String> = scanned.iter().map(|g| g.game_path.clone()).collect();
    let first_seen = first_seen::record_and_load(&app_data_dir(&app), &paths);
    scanner::merge_scanned_games(scanned, &cfg, &first_seen)
}

#[tauri::command]
fn browse_folder(path: Option<String>, file_extensions: Option<Vec<String>>) -> Result<browse::BrowseResult, String> {
    browse::browse_folder(path.as_deref(), file_extensions.as_deref())
}

// ---- Emulator discovery and launch -----------------------------------------

#[tauri::command]
fn find_emulator(app: tauri::AppHandle, state: State<AppState>) -> Result<emulator::EmulatorInfo, String> {
    let binary = emulator_binary(&app, &state)?;
    let version = emulator::probe_version(&binary).ok_or("Emulator did not report a version")?;
    Ok(emulator::EmulatorInfo { path: binary.to_string_lossy().to_string(), version })
}

#[tauri::command]
fn set_emulator_path_override(app: tauri::AppHandle, path: Option<String>) -> Result<(), String> {
    let dir = app_data_dir(&app);
    let mut p = prefs::load(&dir);
    p.emulator_path_override = path;
    prefs::save(&dir, &p).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_prefs(app: tauri::AppHandle) -> prefs::LauncherPrefs {
    prefs::load(&app_data_dir(&app))
}

#[tauri::command]
fn save_prefs(app: tauri::AppHandle, prefs: prefs::LauncherPrefs) -> Result<(), String> {
    prefs::save(&app_data_dir(&app), &prefs).map_err(|e| e.to_string())
}

#[tauri::command]
fn run_game(
    app: tauri::AppHandle,
    state: State<AppState>,
    info: Configuration,
    title_id: String,
) -> Result<(), String> {
    if emulator::is_running(&state.run_state) {
        return Err("A game is already running.".to_string());
    }

    let binary = emulator_binary(&app, &state)?;
    let dir = working_dir(&app, &state);
    let patch_plan = patches::patch_plan_path(&dir, &title_id);
    let gamepad_cfg = config::load(&config::resolve_settings_path());
    let args = emulator::build_args(
        &info,
        Some(&patch_plan),
        &gamepad_cfg.gamepad_keymap,
        gamepad_cfg.gamepad_deadzone,
    );

    let launcher_prefs = prefs::load(&app_data_dir(&app));

    // Auto-close only applies to in-app launches: external-terminal mode
    // already runs the emulator fully independent of the launcher process,
    // so there is nothing here for it to hand off.
    if launcher_prefs.launch_mode == prefs::LaunchMode::InApp && launcher_prefs.auto_close_on_launch {
        let spec = supervisor::SupervisedLaunch {
            interpreter: binary,
            args,
            working_dir: dir,
            log_path: logs::new_session_path(&app_data_dir(&app))
                .unwrap_or_else(|| app_data_dir(&app).join("emulator-session.log")),
            app_data_dir: app_data_dir(&app),
            game_path: info.game_path.clone(),
        };
        // Record the start *before* handing off. Both processes do a
        // read-modify-write of the same playtime.json, and the supervisor's
        // matching `record_stop` fires the moment the emulator exits --
        // which can be almost immediately, if the game crashes on launch.
        // Writing the start first means the supervisor always reads a file
        // that already has this session's `play_count` bump in it, instead
        // of racing this process and losing whichever write lands second.
        let _ = playtime::record_start(&app_data_dir(&app), &info.game_path);
        supervisor::spawn(&spec).map_err(|e| e.to_string())?;
        app.exit(0);
        return Ok(());
    }

    let result = match launcher_prefs.launch_mode {
        prefs::LaunchMode::InApp => {
            let session_log = logs::new_session_path(&app_data_dir(&app))
                .and_then(|path| logs::SessionLog::create(&path));
            if let Some(log) = session_log.as_ref() {
                log.write_header(&info.game_path, &args);
            }
            emulator::spawn_in_app(
                app.clone(),
                state.run_state.clone(),
                &binary,
                &args,
                &dir,
                session_log,
            )
            .map_err(|e| e.to_string())
        }
        prefs::LaunchMode::Terminal => {
            emulator::launch_external_terminal(&binary, &args, &dir).map_err(|e| e.to_string())
        }
    };

    if result.is_ok() {
        let _ = playtime::record_start(&app_data_dir(&app), &info.game_path);
    }
    result
}

/// Absolute path of the session-log folder, for the Console page's "open
/// logs folder" action -- the thing a user attaches to a bug report.
#[tauri::command]
fn get_logs_dir(app: tauri::AppHandle) -> String {
    let dir = logs::logs_dir(&app_data_dir(&app));
    let _ = std::fs::create_dir_all(&dir);
    dir.to_string_lossy().to_string()
}

#[tauri::command]
fn get_play_history(app: tauri::AppHandle) -> playtime::PlayHistory {
    playtime::load(&app_data_dir(&app))
}

#[tauri::command]
fn get_library_stats(app: tauri::AppHandle) -> playtime::LibraryStats {
    playtime::library_stats(&playtime::load(&app_data_dir(&app)))
}

/// Called by the frontend once it observes `emulator-exited` (the process
/// lifetime is only known to `emulator.rs`'s in-app watcher, which has no
/// game identity of its own) — adds the elapsed session length to that
/// game's `total_seconds`. A no-op for the external-terminal launch mode,
/// where the Tauri app never learns the emulator exited.
#[tauri::command]
fn record_play_stop(app: tauri::AppHandle, game_path: String) -> Result<(), String> {
    let dir = app_data_dir(&app);
    let history = playtime::load(&dir);
    let elapsed_seconds = history
        .get(&game_path)
        .map(|s| {
            let now = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis() as u64)
                .unwrap_or(0);
            now.saturating_sub(s.last_played_ms) / 1000
        })
        .unwrap_or(0);
    playtime::record_stop(&dir, &game_path, elapsed_seconds).map_err(|e| e.to_string())
}

#[tauri::command]
fn stop_game(state: State<AppState>) -> Result<(), String> {
    emulator::stop(&state.run_state).map_err(|e| e.to_string())
}

#[tauri::command]
fn is_game_running(state: State<AppState>) -> bool {
    emulator::is_running(&state.run_state)
}

/// Whether this instance was relaunched by `supervisor.rs` after an
/// auto-close-on-launch game session ended, rather than a normal cold
/// start -- the frontend uses this to skip BootController's splash
/// animation, since the user just watched it a few seconds ago on the way
/// into the game.
#[tauri::command]
fn is_resumed_launch() -> bool {
    std::env::var_os("KYTY_RESUMED").is_some()
}

// ---- Saved data -------------------------------------------------------------

#[tauri::command]
fn get_save_data_dirs(app: tauri::AppHandle, state: State<AppState>, title_id: String) -> Vec<String> {
    savedata::find_save_data_dirs(&working_dir(&app, &state), &title_id)
}

#[tauri::command]
fn remove_save_data(dirs: Vec<String>) -> Vec<String> {
    savedata::remove_save_data(&dirs)
}

// ---- Patches ------------------------------------------------------------

#[tauri::command]
fn is_patchable(title_id: String) -> bool {
    patches::is_supported_title_id(&title_id)
}

#[tauri::command]
fn get_patches(app: tauri::AppHandle, state: State<AppState>, title_id: String) -> patches::PatchStatus {
    let path = patches::patch_plan_path(&working_dir(&app, &state), &title_id);
    patches::load_patches(&path)
}

#[tauri::command]
fn save_patches(
    app: tauri::AppHandle,
    state: State<AppState>,
    title_id: String,
    enabled: Vec<bool>,
) -> Result<(), String> {
    let path = patches::patch_plan_path(&working_dir(&app, &state), &title_id);
    patches::save_patches(&path, &enabled)
}

// ---- Trophies -----------------------------------------------------------

#[tauri::command]
fn has_trophy_data(basedir: String) -> bool {
    trophy::has_trophy_data(&basedir)
}

#[tauri::command]
fn get_trophies(app: tauri::AppHandle, basedir: String) -> Result<Vec<trophy::TrophySet>, String> {
    let cache_dir = app_data_dir(&app).join("trophy_cache");
    trophy::load_trophies(&basedir, &cache_dir)
}

/// Trophy counts by grade for one game -- backs the dashboard/Profile trophy
/// OSD (Home.tsx, Profile.tsx). See trophy::TrophyCounts' doc: these are
/// definitions the pack declares, not unlocks (nothing tracks those).
#[tauri::command]
fn get_trophy_counts(basedir: String) -> trophy::TrophyCounts {
    trophy::count_trophies(&basedir)
}

/// Batch form for Profile's library-wide total -- one call instead of one
/// per game in the library.
#[tauri::command]
fn get_trophy_counts_batch(basedirs: Vec<String>) -> HashMap<String, trophy::TrophyCounts> {
    basedirs.into_iter().map(|b| { let c = trophy::count_trophies(&b); (b, c) }).collect()
}

// ---- Compatibility database -----------------------------------------------

#[tauri::command]
fn compatibility_snapshot(state: State<AppState>) -> CompatibilityMap {
    state.compatibility.lock().unwrap().clone()
}

/// Whether this session was launched with `--local` (compatibility editing
/// enabled, backed by a local `compatibility_db.json`) rather than the
/// read-only remote community feed.
#[tauri::command]
fn compatibility_is_local(state: State<AppState>) -> bool {
    state.compatibility_is_local
}

#[tauri::command]
fn compatibility_refresh(app: tauri::AppHandle, state: State<AppState>) -> Result<CompatibilityMap, String> {
    let loaded = if state.compatibility_is_local {
        compatibility::load_local(&working_dir(&app, &state))
    } else {
        compatibility::download_remote()?
    };
    *state.compatibility.lock().unwrap() = loaded.clone();
    Ok(loaded)
}

#[tauri::command]
fn compatibility_set_status(
    app: tauri::AppHandle,
    state: State<AppState>,
    title_id: String,
    status: compatibility::GameStatus,
) -> Result<(), String> {
    if !state.compatibility_is_local {
        return Err("Compatibility editing requires local mode (--local).".to_string());
    }
    let mut entries = state.compatibility.lock().unwrap();
    compatibility::set_status(&mut entries, &title_id, status);
    compatibility::save_local(&working_dir(&app, &state), &entries).map_err(|e| e.to_string())
}

#[tauri::command]
fn compatibility_set_comment(
    app: tauri::AppHandle,
    state: State<AppState>,
    title_id: String,
    comment: String,
) -> Result<(), String> {
    if !state.compatibility_is_local {
        return Err("Compatibility editing requires local mode (--local).".to_string());
    }
    let mut entries = state.compatibility.lock().unwrap();
    compatibility::set_comment(&mut entries, &title_id, comment);
    compatibility::save_local(&working_dir(&app, &state), &entries).map_err(|e| e.to_string())
}

// ---- Art ------------------------------------------------------------------

#[tauri::command]
fn set_game_art(app: tauri::AppHandle, art_key: String, source_path: String) -> Result<String, String> {
    art::set_game_art(&app_data_dir(&app), &art_key, Path::new(&source_path)).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_game_art(app: tauri::AppHandle, art_key: String) -> Option<String> {
    art::get_game_art(&app_data_dir(&app), &art_key)
}

#[tauri::command]
fn remove_game_art(app: tauri::AppHandle, art_key: String) -> Result<(), String> {
    art::remove_game_art(&app_data_dir(&app), &art_key).map_err(|e| e.to_string())
}

// ---- Theme ------------------------------------------------------------------

/// The GNOME/Yaru desktop's `color-scheme` key is the authoritative signal
/// on Linux (checked with `gsettings`, which fails harmlessly to `Err` on
/// desktops without it); the frontend falls back to the Tauri window theme
/// API (cross-platform) when this returns nothing.
#[tauri::command]
fn system_color_scheme() -> Option<String> {
    #[cfg(target_os = "linux")]
    {
        let output = std::process::Command::new("gsettings")
            .args(["get", "org.gnome.desktop.interface", "color-scheme"])
            .output()
            .ok()?;
        let text = String::from_utf8_lossy(&output.stdout).trim().to_lowercase();
        if text.contains("prefer-dark") {
            return Some("dark".to_string());
        }
        if text.contains("prefer-light") || text.contains("default") {
            return Some("light".to_string());
        }
        None
    }
    #[cfg(not(target_os = "linux"))]
    {
        None
    }
}

// ---- Gamepad names -----------------------------------------------------------

/// Names of currently connected gamepads, read natively (like Ryujinx's own
/// SDL2-based enumeration) rather than through the browser Gamepad API the
/// frontend otherwise uses for actual input capture. The Gamepad API cannot
/// answer this by itself: for privacy, `navigator.getGamepads()` reports a
/// pad's presence only after the user has pressed one of its buttons at
/// least once, so a controller that is plugged in but untouched is
/// invisible to it -- confirmed on this exact setup (a connected Xbox
/// controller stayed unnamed in the Input Device dropdown until first
/// pressed). `/proc/bus/input/devices` has no such restriction: a `js`
/// handler is Linux's own marker for "this input device is a joystick",
/// present the moment the kernel enumerates it.
#[tauri::command]
fn list_gamepad_names() -> Vec<String> {
    #[cfg(target_os = "linux")]
    {
        let Ok(text) = std::fs::read_to_string("/proc/bus/input/devices") else {
            return Vec::new();
        };
        let mut names = Vec::new();
        let mut current_name: Option<String> = None;
        for line in text.lines() {
            if let Some(rest) = line.strip_prefix("N: Name=") {
                current_name = Some(rest.trim_matches('"').to_string());
            } else if line.starts_with("H: Handlers=") {
                let is_joystick = line.split_whitespace().any(|tok| tok.starts_with("js"));
                if is_joystick {
                    if let Some(name) = current_name.take() {
                        names.push(name);
                    }
                }
            } else if line.is_empty() {
                current_name = None;
            }
        }
        names
    }
    #[cfg(not(target_os = "linux"))]
    {
        Vec::new()
    }
}

/// One button in NativeGamepadState.buttons, index-aligned to the W3C
/// "Standard Gamepad" layout the frontend already speaks (nav/
/// useGamepadActions.ts, components/InputMappingDialog.tsx's
/// GAMEPAD_BUTTON_NAMES/GAMEPAD_AXIS_NAMES) -- mirrors the shape of a
/// browser `GamepadButton` (`{pressed, value}`) on purpose, so adopting
/// this native source cost the frontend an input-source swap, not a
/// rewrite of the index-based logic built on top of it.
#[derive(serde::Serialize)]
struct NativeButtonState {
    pressed: bool,
    value: f32,
}

#[derive(serde::Serialize)]
struct NativeGamepadState {
    name: String,
    /// Length 17, order: South, East, West, North, LeftBumper, RightBumper,
    /// LeftTrigger(analog), RightTrigger(analog), Select, Start, LeftThumb,
    /// RightThumb, DPadUp, DPadDown, DPadLeft, DPadRight, Mode.
    buttons: Vec<NativeButtonState>,
    /// Length 4: leftX, leftY, rightX, rightY.
    axes: Vec<f32>,
}

/// Replaces the browser Gamepad API (`navigator.getGamepads()`) as the
/// input source for both in-app gamepad navigation and the remap-capture
/// screen. WebKitGTK's Gamepad API depends on `libmanette`, which is not
/// installed on every system (confirmed: this exact machine reported "no
/// Gamepad API support" with a physically connected, working Xbox
/// controller until this was added), and even where it is, that API's
/// presence-privacy rule means a connected-but-untouched controller is
/// invisible until its first button press -- both real, observed failures
/// this app cannot afford for a couch/gamepad-only navigation flow. `gilrs`
/// (also used by, among others, plenty of Rust game engines; conceptually
/// the same role SDL2's game controller subsystem plays for Ryujinx) reads
/// the same kernel evdev/joystick devices directly and has neither
/// limitation. Polled from the frontend on an interval, same architecture
/// as list_gamepad_names and the app's existing rAF-driven input polling --
/// see useGamepadActions.ts and InputMappingDialog.tsx's
/// GamepadCaptureOverlay, both of which call this instead of
/// navigator.getGamepads() now.
#[tauri::command]
fn poll_gamepad_state(state: State<AppState>) -> Vec<NativeGamepadState> {
    use gilrs::{Axis, Button};

    let Some(gilrs_mutex) = &state.gilrs else {
        return Vec::new();
    };
    let Ok(mut gilrs) = gilrs_mutex.lock() else {
        return Vec::new();
    };
    // Drain pending events first -- is_pressed()/value() below read cached
    // state that only updates as events are processed, per gilrs's own
    // documented usage pattern.
    while gilrs.next_event().is_some() {}

    let button = |gp: &gilrs::Gamepad, btn: Button| NativeButtonState {
        pressed: gp.is_pressed(btn),
        value: gp
            .button_data(btn)
            .map(|d| d.value())
            .unwrap_or(if gp.is_pressed(btn) { 1.0 } else { 0.0 }),
    };

    gilrs
        .gamepads()
        .map(|(_, gp)| NativeGamepadState {
            name: gp.name().to_string(),
            buttons: vec![
                button(&gp, Button::South),
                button(&gp, Button::East),
                button(&gp, Button::West),
                button(&gp, Button::North),
                button(&gp, Button::LeftTrigger),
                button(&gp, Button::RightTrigger),
                button(&gp, Button::LeftTrigger2),
                button(&gp, Button::RightTrigger2),
                button(&gp, Button::Select),
                button(&gp, Button::Start),
                button(&gp, Button::LeftThumb),
                button(&gp, Button::RightThumb),
                button(&gp, Button::DPadUp),
                button(&gp, Button::DPadDown),
                button(&gp, Button::DPadLeft),
                button(&gp, Button::DPadRight),
                button(&gp, Button::Mode),
            ],
            axes: vec![
                gp.value(Axis::LeftStickX),
                gp.value(Axis::LeftStickY),
                gp.value(Axis::RightStickX),
                gp.value(Axis::RightStickY),
            ],
        })
        .collect()
}

// ---- Audio devices ------------------------------------------------------------

#[tauri::command]
fn list_audio_sinks() -> Vec<audio::AudioDevice> {
    audio::list_devices(audio::Direction::Output)
}

/// Microphones and other capture endpoints, for games that record input.
#[tauri::command]
fn list_audio_sources() -> Vec<audio::AudioDevice> {
    audio::list_devices(audio::Direction::Input)
}

#[tauri::command]
fn set_audio_output_sink(sink: Option<String>) -> Result<(), String> {
    audio::set_device(audio::Direction::Output, sink)
}

#[tauri::command]
fn set_audio_input_source(source: Option<String>) -> Result<(), String> {
    audio::set_device(audio::Direction::Input, source)
}

/// False where the OS gives a launcher no way to route a child process's
/// audio (Windows). The Audio page shows an explanation instead of a
/// picker that would quietly do nothing.
#[tauri::command]
fn audio_selection_supported() -> bool {
    audio::selection_is_supported()
}

// THROWAWAY — diagnosing the "Run does not launch" report. Removed once fixed.
#[tauri::command]
fn debug_probe_launch(app: tauri::AppHandle, state: State<AppState>) -> String {
    let mut out = String::new();
    match std::env::current_exe() {
        Ok(p) => out.push_str(&format!("current_exe: {p:?}\n")),
        Err(e) => out.push_str(&format!("current_exe ERROR: {e}\n")),
    }
    match emulator_binary(&app, &state) {
        Ok(p) => out.push_str(&format!("emulator_binary: OK {p:?} exists={}\n", p.is_file())),
        Err(e) => out.push_str(&format!("emulator_binary: ERR {e}\n")),
    }
    out.push_str(&format!("is_running: {}\n", emulator::is_running(&state.run_state)));
    let dir = working_dir(&app, &state);
    out.push_str(&format!("working_dir: {dir:?}\n"));
    let prefs = prefs::load(&app_data_dir(&app));
    out.push_str(&format!("launch_mode: {:?}\n", prefs.launch_mode));
    let _ = std::fs::write("/tmp/kyty_launch_probe.txt", &out);
    out
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            // window.set_icon() sets the X11 _NET_WM_ICON hint (and the
            // Windows taskbar/alt-tab icon) -- real fixes for those
            // platforms, dev or bundled. It does NOT fix the GNOME dock on
            // Wayland: GNOME Shell resolves a window's icon by matching its
            // xdg-shell app_id ("kyty-launcher", the Cargo binary name,
            // since enableGTKAppId is unset in tauri.conf.json) against an
            // installed .desktop file's Icon= line, ignoring per-window
            // icon hints entirely. That path is fixed by
            // scripts/install-desktop-entry.sh, which installs
            // kyty-launcher.desktop + this same icon into
            // ~/.local/share/{applications,icons/hicolor}.
            let icon = tauri::image::Image::from_bytes(include_bytes!("../icons/128x128.png"))?;
            if let Some(window) = app.get_webview_window("main") {
                window.set_icon(icon)?;
                #[cfg(target_os = "linux")]
                webkit_tuning::apply(&window);
            }
            // Dedicated always-on gamepad-to-navigation-intent thread (see
            // gamepad.rs's module doc) -- separate from AppState.gilrs,
            // which stays reserved for InputMappingDialog's on-demand raw
            // remap-capture polling.
            gamepad::spawn(app.handle().clone());
            Ok(())
        })
        .manage(AppState {
            run_state: Arc::new(emulator::RunState::default()),
            compatibility: Mutex::new(CompatibilityMap::new()),
            compatibility_is_local: std::env::args().any(|a| a == "--local"),
            gilrs: gilrs::Gilrs::new().ok().map(Mutex::new),
        })
        .invoke_handler(tauri::generate_handler![
            load_config,
            save_config,
            scan_games,
            browse_folder,
            find_emulator,
            set_emulator_path_override,
            get_prefs,
            save_prefs,
            run_game,
            stop_game,
            is_game_running,
            is_resumed_launch,
            get_logs_dir,
            get_play_history,
            get_library_stats,
            record_play_stop,
            get_save_data_dirs,
            remove_save_data,
            is_patchable,
            get_patches,
            save_patches,
            has_trophy_data,
            get_trophies,
            get_trophy_counts,
            get_trophy_counts_batch,
            compatibility_snapshot,
            compatibility_is_local,
            compatibility_refresh,
            compatibility_set_status,
            compatibility_set_comment,
            set_game_art,
            get_game_art,
            remove_game_art,
            system_color_scheme,
            list_gamepad_names,
            poll_gamepad_state,
            list_audio_sinks,
            list_audio_sources,
            set_audio_output_sink,
            set_audio_input_source,
            audio_selection_supported,
            bluetooth::bluetooth_adapter_state,
            bluetooth::list_bluetooth_devices,
            bluetooth::scan_bluetooth_devices,
            bluetooth::pair_bluetooth_device,
            bluetooth::connect_bluetooth_device,
            bluetooth::disconnect_bluetooth_device,
            bluetooth::forget_bluetooth_device,
            debug_probe_launch,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Kyty Launcher");
}
