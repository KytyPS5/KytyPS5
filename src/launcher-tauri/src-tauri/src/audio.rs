//! Audio device enumeration and selection, backing src/lib/audioSettings.ts's
//! Settings > Audio category. Covers both directions: output (what the game
//! plays through) and input (the microphone a game can record from).
//!
//! What "selecting" a device can actually mean is not the same on every OS,
//! and the difference is visible in the UI rather than papered over:
//!
//! - Linux: PulseAudio/PipeWire honour `PULSE_SINK` and `PULSE_SOURCE` per
//!   process. Setting them on *this* process is enough, because emulator.rs
//!   spawns the emulator as a child that inherits this environment. Nothing
//!   touches the OS-wide default, so picking a device here never changes
//!   what the rest of the desktop plays through.
//! - Windows: devices enumerate fine (WinRT device enumeration), but there
//!   is no per-process routing a launcher can apply to a child it did not
//!   write. Windows exposes per-app device assignment only through its own
//!   Settings UI, or to an app that selects its own endpoint -- which
//!   kyty_emulator does not, and which no command-line flag exposes. So the
//!   list is real and selection reports plainly that it cannot take effect.
//! - macOS: same story as Windows -- CoreAudio enumerates real devices
//!   (`coreaudio-rs`'s `macos_helpers`) but has no per-process output
//!   override analogous to `PULSE_SINK`, only a system-wide default this
//!   app deliberately never touches (see `selection_is_supported` below).
//!
//! Enumeration shells out to `pactl` on Linux rather than taking a
//! dependency, following the same convention as lib.rs's system_color_scheme
//! (`gsettings`): its JSON output is trivial to parse and it is the standard
//! CLI on any Linux desktop with audio.

use serde::Serialize;

#[derive(Serialize)]
pub struct AudioDevice {
    /// Stable identifier, and the value handed back to `set_*_device`.
    pub name: String,
    /// What the picker shows.
    pub description: String,
}

/// Which half of the audio path a call refers to. The two are symmetric
/// everywhere except the environment variable and the `pactl` noun.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    Output,
    Input,
}

impl Direction {
    #[cfg(target_os = "linux")]
    fn pactl_noun(self) -> &'static str {
        match self {
            Direction::Output => "sinks",
            Direction::Input => "sources",
        }
    }

    #[cfg(target_os = "linux")]
    fn env_var(self) -> &'static str {
        match self {
            Direction::Output => "PULSE_SINK",
            Direction::Input => "PULSE_SOURCE",
        }
    }

    #[cfg(target_os = "macos")]
    fn coreaudio_scope(self) -> coreaudio::audio_unit::Scope {
        match self {
            Direction::Output => coreaudio::audio_unit::Scope::Output,
            Direction::Input => coreaudio::audio_unit::Scope::Input,
        }
    }
}

/// Best-effort everywhere: an empty list is a normal answer (no audio
/// hardware, `pactl` missing, not on PulseAudio), and the frontend already
/// treats it as one by falling back to just "System default".
#[cfg(target_os = "linux")]
pub fn list_devices(direction: Direction) -> Vec<AudioDevice> {
    let Ok(output) = std::process::Command::new("pactl")
        .args(["-f", "json", "list", direction.pactl_noun()])
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
    let Some(entries) = json.as_array() else {
        return Vec::new();
    };
    entries
        .iter()
        .filter_map(|entry| {
            let name = entry.get("name")?.as_str()?.to_string();
            let description = entry
                .get("description")
                .and_then(|d| d.as_str())
                .unwrap_or(&name)
                .to_string();
            Some(AudioDevice { name, description })
        })
        .collect()
}

#[cfg(windows)]
pub fn list_devices(direction: Direction) -> Vec<AudioDevice> {
    use windows::Devices::Enumeration::{DeviceClass, DeviceInformation};

    let class = match direction {
        Direction::Output => DeviceClass::AudioRender,
        Direction::Input => DeviceClass::AudioCapture,
    };

    // WinRT enumeration is async and its handles are not `Send`, so it runs
    // to completion on a throwaway single-threaded runtime here rather than
    // making every caller async for a list that is read once when a settings
    // page opens.
    let Ok(runtime) = tokio::runtime::Builder::new_current_thread().enable_all().build() else {
        return Vec::new();
    };
    runtime.block_on(async move {
        let Ok(operation) = DeviceInformation::FindAllAsyncDeviceClass(class) else {
            return Vec::new();
        };
        let Ok(collection) = operation.await else {
            return Vec::new();
        };
        collection
            .into_iter()
            .filter_map(|info| {
                // Disabled and unplugged endpoints still enumerate; listing
                // one the user cannot actually pick is worse than omitting it.
                if !info.IsEnabled().unwrap_or(false) {
                    return None;
                }
                let name = info.Id().ok()?.to_string_lossy();
                let description = info
                    .Name()
                    .map(|n| n.to_string_lossy())
                    .ok()
                    .filter(|n| !n.is_empty())
                    .unwrap_or_else(|| name.clone());
                Some(AudioDevice { name, description })
            })
            .collect()
    })
}

/// CoreAudio device ids are stable within a boot but are plain integers
/// with no inherent display form, so the id itself (stringified) is the
/// `name`/identifier and `get_device_name` supplies the description --
/// same split as the Linux pactl backend's name/description pair.
#[cfg(target_os = "macos")]
pub fn list_devices(direction: Direction) -> Vec<AudioDevice> {
    use coreaudio::audio_unit::macos_helpers::{get_audio_device_ids_for_scope, get_device_name};

    let Ok(ids) = get_audio_device_ids_for_scope(direction.coreaudio_scope()) else {
        return Vec::new();
    };
    ids.into_iter()
        .filter_map(|id| {
            let description = get_device_name(id).ok()?;
            Some(AudioDevice { name: id.to_string(), description })
        })
        .collect()
}

/// `device` is validated against `list_devices`'s own output before being
/// set, since it becomes part of an environment the emulator inherits --
/// this rejects an arbitrary or stale frontend-supplied string rather than
/// setting the variable to whatever it happens to be.
#[cfg(target_os = "linux")]
pub fn set_device(direction: Direction, device: Option<String>) -> Result<(), String> {
    let variable = direction.env_var();
    match device {
        None => {
            std::env::remove_var(variable);
            Ok(())
        }
        Some(name) => {
            if !list_devices(direction).iter().any(|d| d.name == name) {
                return Err(format!("unknown audio device: {name}"));
            }
            std::env::set_var(variable, &name);
            Ok(())
        }
    }
}

/// Windows has no per-process audio routing a launcher can apply to a child
/// process. Clearing the choice is fine (nothing was applied in the first
/// place); choosing one says so rather than silently doing nothing.
#[cfg(windows)]
pub fn set_device(_direction: Direction, device: Option<String>) -> Result<(), String> {
    match device {
        None => Ok(()),
        Some(_) => Err(
            "Windows cannot route one app's audio from another. Assign the device to kyty_emulator \
             in Windows Settings > System > Sound > Volume mixer."
                .to_string(),
        ),
    }
}

/// CoreAudio has no per-process output override analogous to `PULSE_SINK`
/// (see this module's doc comment), so selecting a device here cannot
/// retarget the emulator's own audio -- same disclosed gap as Windows,
/// reported the same way rather than silently doing nothing.
#[cfg(target_os = "macos")]
pub fn set_device(_direction: Direction, device: Option<String>) -> Result<(), String> {
    match device {
        None => Ok(()),
        Some(_) => Err(
            "macOS cannot route one app's audio from another. Assign the device to kyty_emulator \
             in System Settings > Sound, or via Audio MIDI Setup."
                .to_string(),
        ),
    }
}

/// Whether picking a device here actually changes where audio goes. The
/// Audio page reads this to decide between a working picker and an
/// explanation, instead of offering a control that quietly does nothing.
pub fn selection_is_supported() -> bool {
    cfg!(target_os = "linux")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enumeration_never_panics_in_either_direction() {
        // Whatever this machine has (or does not have), listing is a
        // question with an answer, never an error.
        let _ = list_devices(Direction::Output);
        let _ = list_devices(Direction::Input);
    }

    #[test]
    fn clearing_a_device_is_always_accepted() {
        assert!(set_device(Direction::Output, None).is_ok());
        assert!(set_device(Direction::Input, None).is_ok());
    }

    #[test]
    fn an_unknown_device_is_rejected() {
        let result = set_device(Direction::Output, Some("definitely-not-a-real-device".to_string()));
        assert!(result.is_err());
    }
}
