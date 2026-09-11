# Kyty Launcher (Tauri UI)

Alternative launcher for KytyPS5, built with Tauri + React instead of Qt Widgets. Fully
drivable with a gamepad: Dashboard, Game library, Game Settings, Profiles, Bluetooth pairing,
gamepad remapping, 19-locale i18n.

This is opt-in and separate. The existing Qt6 launcher (`src/launcher`) is untouched and
stays the one that ships. Nothing in this directory is wired into the root `CMakeLists.txt`
or the CI build (`.github/workflows/build.yml`); building or running the emulator is
unaffected either way.

## Requirements

- Node.js
- Rust toolchain (`cargo`)

## Run it

```bash
cd src/launcher-tauri
npm install       # first time only
./start.sh        # builds the release binary if needed, then runs it
```

`./start.sh --dev` runs `npm run tauri dev` instead (Vite hot reload on `:1421` plus a debug
binary). `./start.sh --no-build` skips the build step and reruns the existing release binary.
See the script's own header comment for why the debug and release binaries are not
interchangeable outside of `tauri dev`.

To build a standalone package:

```bash
npm run tauri build
```

Produces installable artifacts (`.deb`/AppImage on Linux, matching targets on other
platforms) under `src-tauri/target/release/bundle/`.

### Desktop entry (Linux)

```bash
scripts/install-desktop-entry.sh
```

Installs a `.desktop` entry + icon for the current user, so GNOME/KDE on Wayland resolve the
running window to a real icon instead of the generic placeholder. Safe to rerun any time the
binary or icon changes.

## Auto-close on launch

On by default (Settings > Emulator Settings). When a game starts, the launcher's own GUI
process (WebKitGTK/WebView2 and all) exits entirely; a small detached supervisor process
owns the emulator as a real OS child, blocks at zero CPU until the game exits, records the
session's playtime, then relaunches the launcher. See
`src-tauri/src/supervisor.rs` for the mechanism. Turn it off in Settings to keep the
launcher's window open while a game runs instead.
