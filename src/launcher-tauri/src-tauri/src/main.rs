// Prevents an additional console window on Windows in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // `--kyty-supervise <spec-file>` is how the auto-close-on-launch feature
    // (see supervisor.rs) hands the emulator to a detached copy of this same
    // binary. That branch must happen here, before `kyty_launcher_lib::run()`
    // ever touches Tauri/GTK/WebKit -- the whole point of the supervisor is
    // to cost nothing while it blocks on the emulator, and initializing a
    // full webview just to immediately not use it would defeat that.
    let args: Vec<String> = std::env::args().collect();
    let spec_path = args
        .iter()
        .position(|a| a == kyty_launcher_lib::supervisor::SUPERVISE_FLAG)
        .and_then(|i| args.get(i + 1));

    if let Some(spec_path) = spec_path {
        kyty_launcher_lib::supervisor::run(spec_path);
        return;
    }

    kyty_launcher_lib::run();
}
