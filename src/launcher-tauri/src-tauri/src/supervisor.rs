//! Optional "auto-close on launch" mode (Emulator Settings, default on).
//!
//! Keeping the launcher's own GUI process resident while a game runs just
//! makes it compete with the emulator for the same GPU/RAM the whole point
//! of closing it is to free up (see the launcher's own measured footprint:
//! several hundred MB and a WebKit process, versus the Qt launcher's
//! ~70MB). So instead of the emulator being *this* process's child (the
//! normal `emulator::spawn_in_app` path), `run_game` hands the launch off
//! to a detached copy of this same binary invoked as
//! `kyty-launcher --kyty-supervise <spec-file>`, then exits the GUI
//! process entirely.
//!
//! That supervisor invocation is intercepted in `main.rs` *before*
//! `kyty_launcher_lib::run()` -- i.e. before any Tauri/GTK/WebKit
//! initialization happens at all -- so it costs nothing beyond a single
//! blocked-on-`wait()` process. It owns the emulator as a real OS child
//! (no PID-polling, no race), so `Child::wait()` blocks at zero CPU until
//! the game exits, then it finishes the playtime bookkeeping the frontend
//! would otherwise have done on the `emulator-exited` event (that event
//! has no one left to receive it -- the GUI that would show it is gone),
//! relaunches a fresh instance of the real launcher, and exits.

use serde::{Deserialize, Serialize};
use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};

pub const SUPERVISE_FLAG: &str = "--kyty-supervise";

#[derive(Debug, Serialize, Deserialize)]
pub struct SupervisedLaunch {
    pub interpreter: PathBuf,
    pub args: Vec<String>,
    pub working_dir: PathBuf,
    /// Where the emulator's stdout/stderr get written -- there is no
    /// in-app console to stream them to once the GUI has exited, and
    /// silently dropping them would throw away real diagnostic evidence.
    pub log_path: PathBuf,
    pub app_data_dir: PathBuf,
    pub game_path: String,
}

/// Writes the handoff spec to a temp file and spawns a detached
/// `--kyty-supervise <spec-path>` copy of the current binary. Does not wait
/// for it: the caller (`run_game`) exits the GUI process right after this
/// returns `Ok`, so nothing here can depend on the current process still
/// being alive afterward.
pub fn spawn(spec: &SupervisedLaunch) -> std::io::Result<()> {
    // The spec names an executable and its arguments, and the supervisor
    // runs whatever it finds there -- so writing it to a predictable path
    // in a world-writable /tmp would be handing any other local account a
    // way to pre-create that path (as a symlink, to redirect this write; or
    // as a file, to choose the binary that then gets executed). `tempfile`
    // creates with O_EXCL under a random name, mode 0600 on Unix, which
    // closes both: the create fails outright if anything is already there,
    // and nothing but this user can read or replace it afterwards.
    let mut file = tempfile::Builder::new()
        .prefix("kyty-supervise-")
        .suffix(".json")
        .tempfile()?;
    file.write_all(serde_json::to_string(spec)?.as_bytes())?;
    file.flush()?;

    // The supervisor is a separate process that outlives this one, so the
    // file has to survive this `TempFile` being dropped; the supervisor
    // deletes it itself as soon as it has read it.
    let spec_path = file.into_temp_path().keep().map_err(|e| e.error)?;

    let exe = std::env::current_exe()?;
    Command::new(exe).arg(SUPERVISE_FLAG).arg(&spec_path).spawn()?;
    Ok(())
}

/// Entry point for `--kyty-supervise <spec-path>`, called from `main.rs`.
/// Reads the spec, owns the emulator as its own child, waits for it,
/// records the session's playtime, then relaunches the real launcher.
pub fn run(spec_path: &str) {
    let Ok(text) = std::fs::read_to_string(spec_path) else { return };
    let _ = std::fs::remove_file(spec_path);
    let Ok(spec) = serde_json::from_str::<SupervisedLaunch>(&text) else { return };

    let log_file = std::fs::File::create(&spec.log_path).ok();
    let mut cmd = Command::new(&spec.interpreter);
    cmd.args(&spec.args).current_dir(&spec.working_dir);
    crate::emulator::hide_console(&mut cmd);
    if let Some(out) = log_file.as_ref().and_then(|f| f.try_clone().ok()) {
        cmd.stdout(Stdio::from(out));
    }
    if let Some(err) = log_file.as_ref().and_then(|f| f.try_clone().ok()) {
        cmd.stderr(Stdio::from(err));
    }

    let started = std::time::Instant::now();
    if let Ok(mut child) = cmd.spawn() {
        let _ = child.wait();
    }
    let elapsed_seconds = started.elapsed().as_secs();
    let _ = crate::playtime::record_stop(&spec.app_data_dir, &spec.game_path, elapsed_seconds);

    if let Ok(exe) = std::env::current_exe() {
        // Tells the fresh instance to skip its boot/splash animation --
        // this is a resume, not a cold start, so the loading screen the
        // user just watched a few seconds ago on the way *into* the game
        // has no reason to play again on the way back out.
        let _ = Command::new(exe).env("KYTY_RESUMED", "1").spawn();
    }
}
