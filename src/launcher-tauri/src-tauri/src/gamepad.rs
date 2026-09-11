//! Background gamepad-to-navigation-intent thread.
//!
//! Replaces the frontend's old architecture (nav/gamepadSource.ts polling
//! `poll_gamepad_state` over Tauri IPC once per animation frame, forever,
//! whether or not a controller is connected) with a dedicated Rust thread
//! that owns its own `gilrs::Gilrs` instance, samples it directly (no IPC,
//! no JSON round trip through the webview bridge, no React render), and
//! pushes only *discrete navigation intents* to the frontend as Tauri
//! events. AppState's own `gilrs` instance and the `poll_gamepad_state`
//! command are untouched -- InputMappingDialog's remap-capture overlay still
//! needs raw per-button/per-axis polling to figure out *which* physical
//! input was just pressed, which a semantic "up"/"confirm" stream cannot
//! answer; that stays a separate, short-lived, on-demand poll while that one
//! modal is open. This thread is the always-on path for actual navigation.
//!
//! Deadzone here is independent from `KytyLauncherGamepad`'s
//! `gamepad_deadzone` (config.rs) -- that setting configures the *emulated*
//! PS5 controller passed to `kyty_emulator` (`--gamepad-deadzone`), a
//! completely different concern from how the launcher's own UI reads stick
//! input for focus navigation. Conflating the two would mean a deadzone
//! tuned for a game's in-game aiming feel silently changing how snappy the
//! launcher's own menus are, or vice versa.

use gilrs::{Axis, Button, Gilrs};
use serde::Serialize;
use std::collections::HashMap;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};

/// The four cardinal directions plus the five non-directional actions
/// FocusNav.tsx's FocusAction type accepts. Kept as one enum (not a
/// Direction + separate button enum) because they all share one repeat-timer
/// state machine below.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum NavAction {
    Up,
    Down,
    Left,
    Right,
    Confirm,
    Back,
    Menu,
    PageUp,
    PageDown,
}

impl NavAction {
    fn as_str(self) -> &'static str {
        match self {
            NavAction::Up => "up",
            NavAction::Down => "down",
            NavAction::Left => "left",
            NavAction::Right => "right",
            NavAction::Confirm => "confirm",
            NavAction::Back => "back",
            NavAction::Menu => "menu",
            NavAction::PageUp => "pageUp",
            NavAction::PageDown => "pageDown",
        }
    }
}

#[derive(Clone, Serialize)]
struct NavIntentPayload {
    action: &'static str,
    seq: u64,
    /// "button" (includes d-pad and triggers) or "stick" -- FocusNav does
    /// not currently branch on this, but it is useful for the diagnostics
    /// overlay (Phase 6) to show what is actually driving navigation.
    source: &'static str,
    /// 0.0-1.0. Always 1.0 for a digital button; the deadzone-rescaled
    /// stick displacement for a stick-sourced direction.
    magnitude: f32,
}

#[derive(Clone, Serialize)]
struct NavScrollPayload {
    x: f32,
    y: f32,
}

#[derive(Clone, Serialize)]
struct NavPadsPayload {
    names: Vec<String>,
}

/// Left-stick reading, treated as a discrete d-pad-style navigation input
/// (owner decision 2026-09-09, carried over unchanged from the previous
/// frontend implementation: no free 2D cursor). Deadzone is *radial*
/// (magnitude first, then direction), not per-axis independent clamping --
/// an independent per-axis deadzone is slightly biased toward diagonals;
/// correct once here rather than working around it downstream.
const STICK_DEADZONE: f32 = 0.35;
/// Once a direction has fired, the raw stick magnitude must fall below this
/// (lower than STICK_DEADZONE) before that direction is considered
/// released. Without this gap, a stick resting almost exactly on the
/// acquisition threshold chatters press/release every tick.
const STICK_RELEASE_DEADZONE: f32 = 0.28;

/// Direction is resolved by full angle (atan2), not by comparing |x| to |y|
/// -- the old dominant-axis comparison made a diagonal push flicker between
/// the two axes as it crossed exactly 45 degrees. This bakes in a hysteresis
/// band around each 90-degree boundary instead: a new direction must beat
/// the currently-latched one by more than this many degrees to take over,
/// so the resolved direction only changes when the stick has genuinely
/// moved toward a different cardinal, not when it wobbles across a line.
const DIAGONAL_HYSTERESIS_DEG: f32 = 12.0;

const TRIGGER_THRESHOLD: f32 = 0.5;

// 02-focus-hover-navigation.md's "practical repeat profile", unchanged from
// the previous frontend implementation -- this thread reproduces the exact
// same feel, just from a source that costs nothing per tick instead of an
// IPC round trip.
const REPEAT_DELAY_MS: u64 = 300;
const REPEAT_INTERVAL_MS: u64 = 90;
const STICK_REPEAT_MAX_MS: f32 = 260.0; // just past the deadzone: steps slowly
const STICK_REPEAT_MIN_MS: f32 = 90.0; // full tilt: sweeps, matching button repeat

/// While any input is held or a stick is deflected, tick fast enough for the
/// 90ms repeat interval to read as smooth (250Hz gives +/-4ms jitter on a
/// 90ms period, imperceptible). At rest, back off to this rate -- still low
/// enough latency that the very first press of a session is never
/// perceptibly delayed, but roughly 15x fewer wakeups than staying at 250Hz
/// forever. This is the actual "near-zero idle cost" this thread delivers:
/// not a literal zero (gilrs has no portable blocking-wait primitive to
/// park on across platforms), but eliminating the IPC round trip, JSON
/// serialization and React render that ran on every one of those ticks
/// before -- that was where nearly all of the old idle cost actually was.
const ACTIVE_TICK: Duration = Duration::from_millis(4);
const IDLE_TICK: Duration = Duration::from_millis(16);

struct RepeatState {
    next_repeat: Instant,
}

/// Applies a radial deadzone to a 2D input: magnitude below `deadzone`
/// reads as dead center; magnitude above it is rescaled so it still spans
/// the full 0..1 range from the deadzone edge outward (Kodi's
/// `DeadzoneFilter::ApplyDeadzone`, generalized from one axis to a vector).
/// Returns (x, y, magnitude) all already rescaled; (0, 0, 0) inside the
/// deadzone.
fn apply_radial_deadzone(x: f32, y: f32, deadzone: f32) -> (f32, f32, f32) {
    let raw_mag = (x * x + y * y).sqrt();
    if raw_mag <= deadzone || raw_mag <= 0.0 {
        return (0.0, 0.0, 0.0);
    }
    let rescaled = ((raw_mag - deadzone) / (1.0 - deadzone)).min(1.0);
    (x / raw_mag * rescaled, y / raw_mag * rescaled, rescaled)
}

/// Resolves a deadzone-applied stick vector to one of the four cardinal
/// NavActions, latching onto `last` under DIAGONAL_HYSTERESIS_DEG so the
/// boundary between two directions does not chatter. `y` is expected in the
/// browser Gamepad API's convention (positive = down) -- callers negate
/// gilrs's native Y before calling this, matching the inversion the old
/// frontend code applied (gilrs-core reports +1 = up on Linux).
fn resolve_stick_direction(x: f32, y: f32, last: Option<NavAction>) -> NavAction {
    let angle = y.atan2(x).to_degrees();
    let angle = if angle < 0.0 { angle + 360.0 } else { angle };
    const CARDINALS: [(f32, NavAction); 4] = [
        (0.0, NavAction::Right),
        (90.0, NavAction::Down),
        (180.0, NavAction::Left),
        (270.0, NavAction::Up),
    ];
    let circular_dist = |a: f32, b: f32| {
        let d = (a - b).abs() % 360.0;
        d.min(360.0 - d)
    };
    let mut best = CARDINALS[0].1;
    let mut best_dist = f32::MAX;
    for &(center, action) in &CARDINALS {
        let mut dist = circular_dist(angle, center);
        if last != Some(action) {
            dist += DIAGONAL_HYSTERESIS_DEG;
        }
        if dist < best_dist {
            best_dist = dist;
            best = action;
        }
    }
    best
}

fn stick_repeat_interval(magnitude: f32) -> Duration {
    let t = magnitude.clamp(0.0, 1.0);
    let ms = STICK_REPEAT_MAX_MS - t * (STICK_REPEAT_MAX_MS - STICK_REPEAT_MIN_MS);
    Duration::from_millis(ms as u64)
}

/// Spawns the thread and returns immediately. No shutdown handle: this
/// thread's only state is in-memory input timing, and the process exiting
/// (the only way this app's main window closes) tears it down for free --
/// adding a stop channel here would be a defensive layer this app does not
/// need.
pub fn spawn(app: AppHandle) {
    std::thread::Builder::new()
        .name("kyty-gamepad-nav".into())
        .spawn(move || run(app))
        .expect("failed to spawn gamepad navigation thread");
}

fn run(app: AppHandle) {
    let Ok(mut gilrs) = Gilrs::new() else {
        // No udev/joystick backend on this system -- navigation still works
        // via keyboard (FocusNav.tsx's keydown handler is independent of
        // this thread entirely). Nothing to log as an error: this is a
        // normal state on a system with no controller support at all.
        return;
    };

    let mut seq: u64 = 0;
    let mut repeats: HashMap<NavAction, RepeatState> = HashMap::new();
    let mut last_stick_dir: Option<NavAction> = None;
    let mut last_scroll_active = false;
    let mut known_pad_names: Vec<String> = Vec::new();

    loop {
        while gilrs.next_event().is_some() {}

        let mut seen: HashMap<NavAction, (&'static str, f32)> = HashMap::new();
        // Merge across every connected pad: any pad pressing a given action
        // counts, and for the stick, whichever pad reports the largest
        // magnitude this tick drives direction resolution.
        let mut best_stick: Option<(f32, f32, f32)> = None; // rescaled x, y, magnitude
        let mut best_right: Option<(f32, f32)> = None;
        let mut pad_names = Vec::new();

        // The stick's own deadzone this tick depends on whether a direction
        // is already latched (hysteresis: release threshold is lower than
        // acquisition threshold), so it must be decided before reading axes.
        let stick_deadzone = if last_stick_dir.is_some() { STICK_RELEASE_DEADZONE } else { STICK_DEADZONE };

        for (_, gp) in gilrs.gamepads() {
            pad_names.push(gp.name().to_string());

            for (button, action) in [
                (Button::South, NavAction::Confirm),
                (Button::East, NavAction::Back),
                (Button::North, NavAction::Menu),
                (Button::DPadUp, NavAction::Up),
                (Button::DPadDown, NavAction::Down),
                (Button::DPadLeft, NavAction::Left),
                (Button::DPadRight, NavAction::Right),
            ] {
                if gp.is_pressed(button) {
                    seen.entry(action).or_insert(("button", 1.0));
                }
            }
            if gp.value(Axis::LeftZ) > TRIGGER_THRESHOLD || gp.is_pressed(Button::LeftTrigger2) {
                seen.entry(NavAction::PageUp).or_insert(("button", 1.0));
            }
            if gp.value(Axis::RightZ) > TRIGGER_THRESHOLD || gp.is_pressed(Button::RightTrigger2) {
                seen.entry(NavAction::PageDown).or_insert(("button", 1.0));
            }

            let lx = gp.value(Axis::LeftStickX);
            let ly = -gp.value(Axis::LeftStickY); // gilrs-core: +1 = up on Linux; flip to the browser convention (+y = down)
            let (dx, dy, mag) = apply_radial_deadzone(lx, ly, stick_deadzone);
            if mag > 0.0 && best_stick.map(|(_, _, m)| mag > m).unwrap_or(true) {
                best_stick = Some((dx, dy, mag));
            }

            let rx = gp.value(Axis::RightStickX);
            let ry = -gp.value(Axis::RightStickY);
            let (rdx, rdy, rmag) = apply_radial_deadzone(rx, ry, STICK_DEADZONE);
            if rmag > 0.0 {
                best_right = Some((rdx, rdy));
            }
        }

        if pad_names != known_pad_names {
            // Naturally rate-limited (fires only on an actual connect/
            // disconnect edge, not per-tick) rather than needing its own
            // counter -- this is the one piece of this thread's state that
            // is worth a log line: "is a pad even seen at all" is the
            // first thing worth checking if navigation reportedly isn't
            // responding to a controller, and this is the only place that
            // fact is available without attaching a debugger.
            for name in pad_names.iter().filter(|n| !known_pad_names.contains(n)) {
                eprintln!("[kyty-gamepad-nav] connected: {name}");
            }
            for name in known_pad_names.iter().filter(|n| !pad_names.contains(n)) {
                eprintln!("[kyty-gamepad-nav] disconnected: {name}");
            }
            known_pad_names = pad_names.clone();
            let _ = app.emit("nav-pads", NavPadsPayload { names: pad_names });
        }

        last_stick_dir = match best_stick {
            Some((dx, dy, mag)) => {
                let dir = resolve_stick_direction(dx, dy, last_stick_dir);
                seen.entry(dir).or_insert(("stick", mag));
                Some(dir)
            }
            None => None,
        };

        let now = Instant::now();
        for (&action, &(source, magnitude)) in seen.iter() {
            match repeats.get_mut(&action) {
                None => {
                    repeats.insert(action, RepeatState { next_repeat: now + Duration::from_millis(REPEAT_DELAY_MS) });
                    seq += 1;
                    let _ = app.emit("nav-intent", NavIntentPayload { action: action.as_str(), seq, source, magnitude });
                }
                Some(state) => {
                    if now >= state.next_repeat {
                        let interval = if source == "stick" { stick_repeat_interval(magnitude) } else { Duration::from_millis(REPEAT_INTERVAL_MS) };
                        // Accumulate off the previous target, not off `now`
                        // -- the old frontend implementation re-armed off
                        // `now`, which silently drops any overshoot on a
                        // late tick. This thread's tick (4ms active) is far
                        // finer than any repeat interval (>=90ms), so a
                        // single `if` rather than a catch-up `while` cannot
                        // fall behind.
                        state.next_repeat += interval;
                        seq += 1;
                        let _ = app.emit("nav-intent", NavIntentPayload { action: action.as_str(), seq, source, magnitude });
                    }
                }
            }
        }
        repeats.retain(|action, _| seen.contains_key(action));

        // Right stick: continuous analog scroll, not a repeated discrete
        // intent. Emitted at the thread's own tick rate while active (which
        // is already the fast 4ms tick, since an active right stick counts
        // as "active" below) -- the frontend's scroll controller integrates
        // it against real elapsed time, so it is not rate-sensitive the way
        // the repeat state machine above is.
        match best_right {
            Some((x, y)) => {
                last_scroll_active = true;
                let _ = app.emit("nav-scroll", NavScrollPayload { x, y });
            }
            None => {
                if last_scroll_active {
                    last_scroll_active = false;
                    let _ = app.emit("nav-scroll", NavScrollPayload { x: 0.0, y: 0.0 });
                }
            }
        }

        let active = !seen.is_empty() || last_scroll_active;
        std::thread::sleep(if active { ACTIVE_TICK } else { IDLE_TICK });
    }
}
