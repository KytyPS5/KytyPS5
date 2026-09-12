// UI sound effects -- generated originals (this file's siblings in
// public/sfx/), not extracted from any real console's system sounds.
// Wired into the focus-navigation system
// (FocusNav.tsx) so sound follows gamepad/keyboard navigation the way the
// real PS5 UI does, not into every mouse click across the app.
//
// Volume/enabled come from lib/audioSettings.ts's store (Settings > Audio),
// not a fixed constant -- read live at play time (not just once at pool
// creation) so a volume change reaches sounds already sitting in the pool,
// per that settings pane's own requirement.

import { audioSettingsStore } from "./audioSettings";

// AAC/.m4a, not Ogg/Opus: Safari/WKWebView has never reliably supported
// Ogg/Opus playback, so on macOS every one of these would fail
// HTMLAudioElement.play() silently by design (this file's own "never let
// audio failure break navigation" contract below), leaving the whole app
// silent with nothing surfaced to the user. AAC/.m4a is supported by
// WebKitGTK, WebView2 and WKWebView alike, so this removes the platform
// gap entirely rather than branching on it.
const FILES = {
  nav: "/sfx/nav_move.m4a",
  confirm: "/sfx/confirm.m4a",
  back: "/sfx/back.m4a",
  boot: "/sfx/boot.m4a",
} as const;

export type SfxName = keyof typeof FILES;

// Each sound's own relative level, preserved as a multiplier against the
// user's overall SFX volume slider (0..1) -- boot stays proportionally
// louder than a nav tick at any slider position, matching the original
// fixed-constant balance this replaced.
const RELATIVE_VOLUME: Record<SfxName, number> = {
  nav: 0.35,
  confirm: 0.5,
  back: 0.4,
  boot: 0.6,
};

// Small round-robin voice pool per sound, not one shared HTMLAudioElement.
// Rapid D-pad repeat (as fast as 90ms flat, down to 90ms at full stick
// tilt -- src-tauri/src/gamepad.rs's REPEAT_INTERVAL_MS/STICK_REPEAT_MIN_MS,
// the actual current source of repeat timing) previously reused ONE element
// and reset `currentTime = 0` on every play, abruptly cutting off whatever
// tail the previous play still had instead of letting successive plays
// overlap naturally the way distinct voices would.
const POOL_SIZE = 3;
const pools = new Map<SfxName, HTMLAudioElement[]>();
const nextVoice = new Map<SfxName, number>();

function getPool(name: SfxName): HTMLAudioElement[] {
  let p = pools.get(name);
  if (!p) {
    p = Array.from({ length: POOL_SIZE }, () => new Audio(FILES[name]));
    pools.set(name, p);
  }
  return p;
}

/** Constructs every sound's voice pool upfront (call once at boot, App.tsx).
 * Without this, the first D-pad move constructed a fresh `Audio` element
 * and kicked off its network fetch + decode in the same tick as
 * FocusNav's setFocus -- this makes navigation, not just boot's own
 * fanfare, exempt from that first-use hitch. */
export function warmSfx(): void {
  for (const name of Object.keys(FILES) as SfxName[]) getPool(name);
}

/** Plays a UI sound from the next voice in that sound's pool, round-robin.
 * Failures (autoplay policy before first user gesture, missing file) are
 * swallowed -- a missing blip must never break navigation. A no-op while
 * SFX are disabled in Settings. */
export function playSfx(name: SfxName): void {
  try {
    const { sfxEnabled, sfxVolume } = audioSettingsStore.get();
    if (!sfxEnabled) return;
    const p = getPool(name);
    const idx = (nextVoice.get(name) ?? 0) % p.length;
    nextVoice.set(name, idx + 1);
    const el = p[idx];
    el.volume = Math.min(1, Math.max(0, sfxVolume)) * RELATIVE_VOLUME[name];
    el.currentTime = 0;
    void el.play().catch(() => undefined);
  } catch {
    // See doc comment -- never let audio failure break the UI.
  }
}
