//! The Windows half of the idle-cost work that `webkit_tuning.rs` does for
//! WebKitGTK. Measured here with the window open and the user doing nothing,
//! this launcher sat at ~17% of one core and ~511MB working set across eight
//! processes (kyty-launcher plus WebView2's browser/renderer/GPU/utility
//! set). Most of the CPU was WebView2's GPU process compositing animations
//! nobody was watching; the frontend's `data-idle` switch (lib/idle.ts) is
//! what parks those.
//!
//! This file covers the other half, the resident memory, for the one case
//! that actually matters to an emulator front-end: a game is running and the
//! launcher is still alive because `autoCloseOnLaunch` is off. WebView2's
//! `MemoryUsageTargetLevel` is Microsoft's documented knob for exactly that
//! -- "Set MemoryUsageTargetLevel = Low on inactive WebViews to reduce memory
//! usage -- this may prompt the browser engine to drop cached data or swap
//! memory to disk", restored to `Normal` when the WebView is active again.
//!
//! Deliberately NOT using `TrySuspendAsync`, the stronger knob sitting right
//! next to it: its documented precondition is that the controller's
//! `IsVisible` be false, and it throws `ERROR_INVALID_STATE` otherwise. The
//! launcher window is normally still visible behind a running game, so using
//! suspend would mean blanking the webview first -- a visible behaviour
//! change, not a free win, and one for the owner to decide rather than
//! something to slip in under a memory tweak.
//!
//! Only bound to the game-running transition, never to focus changes: Low is
//! documented to drop caches and swap, so paying that on every alt-tab would
//! trade a little idle RAM for a stutter on every return.

use webview2_com::Microsoft::Web::WebView2::Win32::{
    ICoreWebView2_19, COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW,
    COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL,
};
use windows_core::Interface;

/// Best effort throughout: every step here is an optional capability of the
/// installed WebView2 runtime (`ICoreWebView2_19` is only present from
/// 1.0.1722.45 on), and failing to shrink a cache is never a reason to fail
/// whatever the caller was actually doing.
pub fn set_low_memory(window: &tauri::WebviewWindow, low: bool) {
    let level = if low {
        COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
    } else {
        COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL
    };

    let _ = window.with_webview(move |webview| {
        let controller = webview.controller();
        let Ok(core) = (unsafe { controller.CoreWebView2() }) else {
            return;
        };
        let Ok(core19) = core.cast::<ICoreWebView2_19>() else {
            return;
        };
        let _ = unsafe { core19.SetMemoryUsageTargetLevel(level) };
    });
}
