/** Idle throttle switch. Sets `data-idle="1"` on the document root whenever
 * this launcher's animation is work nobody is watching, and CSS (theme.css's
 * `---- idle throttle` block, plus the panel modules) reacts by parking every
 * running animation and dropping live backdrop-filter to a flat fill.
 *
 * Why: measured on Windows with the window open and the user doing nothing,
 * the launcher sat at ~17% of one core -- ~13% of that in WebView2's GPU
 * process -- and never went quiet, because the hero Ken Burns pan
 * (HeroBackground.module.css, 42s infinite alternate), the per-tile flux
 * spin/shimmer (GameTile/GameCard, 2.4s + 5s infinite) and the in-game pulse
 * dot all run forever, compositing against 34 backdrop-filter rules. None of
 * that is worth a single frame while the window is hidden, unfocused, or --
 * the case this launcher exists to serve -- while a game is running and the
 * emulator wants every core and every slice of GPU it can get.
 *
 * Four inputs, OR'd together:
 *
 * - `document.hidden` -- fully occluded. The webview already throttles rAF
 *   here, but CSS animations are driven by the compositor and keep going, so
 *   this still needs saying out loud.
 * - the native window being minimized, asked of Tauri rather than inferred.
 *   Measured: minimizing does not reliably produce a `blur` in the webview,
 *   and `document.hidden` stays false, so a launcher minimized straight from
 *   a focused state kept animating at ~11% of a core. This is the signal
 *   that catches it.
 * - window focus -- the common case. Alt-tab away and the launcher is not
 *   being looked at even though it is still visible.
 * - a game running -- the important one. With `autoCloseOnLaunch` off the
 *   launcher stays resident for the whole session, so without this it spends
 *   the entire game competing with the emulator for the GPU.
 *
 * Deliberately does NOT touch the rAF loops: nav/gamepadSource.ts's shared
 * poll is what notices the pad being picked back up, so pausing it would
 * strand an unfocused launcher with no way to wake on the pad.
 *
 * Paused animations resume exactly where they stopped, so an entrance caught
 * mid-flight finishes correctly on the way back rather than snapping.
 */

import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { isRunningStore } from "../store/run";

// Seeded from the real state rather than assumed `true`: a window that is
// launched into the background, or that loses focus before this module runs,
// never fires the `blur` that would otherwise be the only thing to correct
// an optimistic default -- measured, that left the launcher animating at
// ~14% of a core while minimized, which is exactly the case this file
// exists to kill. `hasFocus()` is synchronous and right often enough to
// avoid a flash; the Tauri query in installIdleThrottle settles it.
let windowFocused = document.hasFocus();
let windowMinimized = false;
let gameRunning = false;

function apply(): void {
  const idle = document.hidden || windowMinimized || !windowFocused || gameRunning;
  if (idle) {
    document.documentElement.dataset.idle = "1";
  } else {
    delete document.documentElement.dataset.idle;
  }
}

export function installIdleThrottle(): void {
  document.addEventListener("visibilitychange", apply);

  // DOM focus/blur is the synchronous signal and fires for native window
  // focus changes in both WebView2 and WebKitGTK. Tauri's own event is
  // subscribed to as well below, as the authoritative one.
  window.addEventListener("focus", () => {
    windowFocused = true;
    apply();
  });
  window.addEventListener("blur", () => {
    windowFocused = false;
    apply();
  });

  isRunningStore.subscribe(() => {
    const next = isRunningStore.get();
    if (next === gameRunning) return;
    gameRunning = next;
    apply();
    // Windows only in practice (no-op elsewhere), and bound to this
    // transition alone rather than to `apply()`: WebView2 documents the low
    // level as dropping cached data and swapping to disk, which is worth it
    // once for the length of a game session and not worth it on every
    // alt-tab. Best effort -- an older WebView2 runtime simply lacks it.
    void invoke("set_webview_memory_low", { low: next }).catch(() => undefined);
  });

  // Everything below is guarded: outside a Tauri host (plain `vite` in a
  // browser, which is how the UI is sometimes poked at in isolation) there
  // is no native window to ask, and the DOM listeners above carry the case
  // on their own.
  const win = (() => {
    try {
      return getCurrentWindow();
    } catch {
      return null;
    }
  })();
  if (!win) return;

  /** Re-reads the native window rather than trusting the webview's own view
   * of it. Both answers are needed: focus alone misses a minimize, and
   * minimized alone misses an alt-tab. */
  const refresh = async () => {
    const [focused, minimized] = await Promise.all([
      win.isFocused().catch(() => windowFocused),
      win.isMinimized().catch(() => windowMinimized),
    ]);
    windowFocused = focused;
    windowMinimized = minimized;
    apply();
  };

  void win.onFocusChanged(({ payload: focused }) => {
    windowFocused = focused;
    apply();
    // Still re-read: a focus change is also when a minimize becomes true.
    void refresh();
  });
  // Minimize and restore both arrive as a resize.
  void win.onResized(() => void refresh());
  void refresh();

  apply();
}
