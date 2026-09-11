//! Cuts WebKitGTK's idle memory footprint for the single always-resident
//! dashboard window this app actually has -- see start.sh's sibling
//! investigation: a stock webview here settles around 850MB RSS
//! (WebKitWebProcess + main + network process) against the previous Qt
//! launcher's ~72MB. Most of that gap is WebKitGTK defaulting every setting
//! to "general-purpose browser", none of which this app is:
//!
//! - `CacheModel::WebBrowser` sizes WebKit's page/object cache off total
//!   system RAM, for a tab that navigates between many pages. This app is
//!   one SPA route tree that never leaves its own origin, so that cache
//!   buys nothing and just inflates the baseline.
//! - The engine subsystems below (WebGL, WebRTC, media capture/streaming,
//!   EME, WebSQL, the offline app-cache) are verified unused by this
//!   frontend (checked: no WebGL canvas context, no
//!   getUserMedia/RTCPeerConnection, no AudioContext). Leaving them enabled
//!   still costs WebKit the initialization/reservation overhead of each
//!   subsystem for nothing.
//! - There is no multi-page navigation in this app at all, so the
//!   back/forward page cache and its navigation-gesture handling are dead
//!   weight, and WebKit's built-in spatial navigation only competes with
//!   this app's own gamepad-driven one (nav/FocusNav.tsx).
//!
//! This does not close the gap to Qt's ~72MB -- most of that is WebKitGTK's
//! own fixed engine overhead (JS engine, layout/paint engine, its own
//! network stack), which no setting removes. It only strips the added cost
//! of browser features this app never uses.

use webkit2gtk::{CacheModel, SettingsExt, WebContextExt, WebViewExt};

pub fn apply(window: &tauri::WebviewWindow) {
    let result = window.with_webview(|webview| {
        let webview = webview.inner();

        if let Some(context) = webview.context() {
            context.set_cache_model(CacheModel::DocumentViewer);
        }

        if let Some(settings) = webview.settings() {
            settings.set_enable_webgl(false);
            settings.set_enable_webrtc(false);
            settings.set_enable_media_stream(false);
            settings.set_enable_mediasource(false);
            settings.set_enable_encrypted_media(false);
            settings.set_enable_media_capabilities(false);
            settings.set_enable_mock_capture_devices(false);
            settings.set_enable_webaudio(false);
            settings.set_enable_html5_database(false);
            settings.set_enable_offline_web_application_cache(false);
            settings.set_enable_page_cache(false);
            settings.set_enable_back_forward_navigation_gestures(false);
            settings.set_enable_spatial_navigation(false);
        }
    });

    if let Err(err) = result {
        eprintln!("webkit_tuning::apply: could not reach the webview to tune it: {err}");
    }
}
