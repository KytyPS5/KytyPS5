//! Audio output device enumeration and selection, backing
//! src/lib/audioSettings.ts's Settings > Audio category.
//!
//! Enumeration shells out to `pactl -f json list sinks`, following the same
//! "shell out directly, no shell plugin" convention as lib.rs's
//! system_color_scheme (`gsettings`) -- PulseAudio/PipeWire's `pactl` is the
//! standard, always-present CLI on any Linux desktop with audio, and its
//! JSON output (`pactl --version` 16+) is trivial to parse without a new
//! dependency.
//!
//! Selection sets `PULSE_SINK` on this process's own environment rather
//! than calling `pactl set-default-sink` (which would change the *system*
//! default for every app, not just this one -- explicitly out of scope,
//! see the owner's "does not touch the OS-wide default sink" decision).
//! Because Tauri runs WebKitGTK in-process on Linux (unlike a multi-process
//! browser), this reaches the launcher's own UI-sound playback directly;
//! kyty_emulator picks it up too, since emulator.rs spawns it as a child
//! process that inherits this process's environment.

use serde::Serialize;

#[derive(Serialize)]
pub struct AudioSink {
    pub name: String,
    pub description: String,
}

/// Best-effort: returns an empty list on any failure (pactl missing,
/// not on PulseAudio/PipeWire, malformed output) rather than an error --
/// the frontend already treats "no sinks" as a normal, unremarkable state
/// (falls back to just "System default" in the picker).
pub fn list_sinks() -> Vec<AudioSink> {
    let Ok(output) = std::process::Command::new("pactl")
        .args(["-f", "json", "list", "sinks"])
        .output()
    else {
        return Vec::new();
    };
    if !output.status.success() {
        return Vec::new();
    }
    let Ok(json) = serde_json::from_slice::<serde_json::Value>(&output.stdout) else {
        return Vec::new();
    };
    let Some(sinks) = json.as_array() else {
        return Vec::new();
    };
    sinks
        .iter()
        .filter_map(|sink| {
            let name = sink.get("name")?.as_str()?.to_string();
            let description = sink
                .get("description")
                .and_then(|d| d.as_str())
                .unwrap_or(&name)
                .to_string();
            Some(AudioSink { name, description })
        })
        .collect()
}

/// `sink` is validated against `list_sinks()`'s own output before being set
/// as a process environment variable, since it becomes part of a process
/// environment other code (kyty_emulator, launched later) inherits --
/// this rejects an arbitrary/stale frontend-supplied string rather than
/// setting PULSE_SINK to whatever it happens to be.
pub fn set_output_sink(sink: Option<String>) -> Result<(), String> {
    match sink {
        None => {
            std::env::remove_var("PULSE_SINK");
            Ok(())
        }
        Some(name) => {
            if !list_sinks().iter().any(|s| s.name == name) {
                return Err(format!("unknown audio sink: {name}"));
            }
            std::env::set_var("PULSE_SINK", &name);
            Ok(())
        }
    }
}
