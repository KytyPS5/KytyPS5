# Running the Kyty Launcher (Tauri) — Windows, macOS, Linux

Step-by-step guide to starting the Tauri launcher (`src/launcher-tauri`) on each platform.

[README.md](README.md) covers what the launcher *is* and the Linux `./start.sh` flow.
This file covers the cross-platform startup path, because `start.sh` is a bash script that
probes `../../_Build/linux/install` and uses `ss` — it does not apply on Windows.

The Tauri launcher is opt-in and separate from the Qt6 launcher (`src/launcher`). It is not
wired into the root `CMakeLists.txt` or CI, so nothing here changes how the emulator builds.

## 1. Prerequisites

| Need | Windows | macOS | Linux |
|---|---|---|---|
| Node.js + npm | [nodejs.org](https://nodejs.org) LTS | `brew install node` | distro package |
| Rust toolchain | `rustup` (`x86_64-pc-windows-msvc`) | `rustup` | `rustup` |
| C/C++ linker | MSVC Build Tools (already present if you build the emulator with `clang-cl`) | Xcode CLT | `build-essential` |
| WebView | WebView2 Runtime — preinstalled on Windows 11 | WKWebView (system) | `webkit2gtk-4.1` + `libsoup3` dev packages |

Check what you have:

```powershell
node --version; npm --version; cargo --version
```

## 2. Build the emulator first (recommended)

The launcher UI starts without it, but **Launch** fails with `Could not find kyty_emulator`
until the emulator binary exists. Build and install it per the root
[README.md](../../README.md):

```powershell
# from the repo root, inside a VS dev shell
cmake --build _Build/windows --target kyty_emulator
cmake --install _Build/windows --prefix _Build/windows/install
```

That produces `_Build/windows/install/kyty_emulator.exe` (`_Build/linux/install/kyty_emulator`
on Linux, `_Build/macos/install/` on macOS) — exactly where the launcher looks. See
[How the launcher finds the emulator](#how-the-launcher-finds-the-emulator).

## 3. Install the frontend dependencies

Once, and again whenever `package.json` changes:

```powershell
cd src\launcher-tauri
npm ci
```

`npm ci` installs the exact `package-lock.json` set. Use `npm install` only when adding deps.

## 4. Start it

Three ways. Pick by what you are doing.

### a. Development — hot reload

```powershell
npm run tauri dev
```

Vite serves the UI on `http://localhost:1421` with HMR, and a debug Tauri binary points at it.
Edit anything under `src/` and the window refreshes. Rust changes under `src-tauri/src/`
trigger a recompile and restart.

Port 1421 is `strictPort: true` in [vite.config.ts](vite.config.ts) — if something else holds
it, the dev server fails instead of picking another port. Find the holder:

```powershell
netstat -ano | Select-String ":1421"
Stop-Process -Id <pid>
```

### b. Release binary — standalone, what you actually play on

```powershell
npm run tauri build -- --no-bundle
.\src-tauri\target\release\kyty-launcher.exe
```

`--no-bundle` skips installer generation, so you get just the executable. On Linux/macOS the
binary is `src-tauri/target/release/kyty-launcher`.

> **The debug binary is not a substitute.** It has `devUrl http://localhost:1421` baked in and
> fails standalone with `Could not connect to localhost: Connection refused`. Only the release
> binary embeds the built frontend. Run the debug one through `tauri dev` or not at all.

### c. Installable package

```powershell
npm run tauri build
```

Artifacts land in `src-tauri/target/release/bundle/`. `bundle.targets` is `"all"` in
[tauri.conf.json](src-tauri/tauri.conf.json), so each host resolves its own formats —
MSI/NSIS on Windows, `.app`/`.dmg` on macOS, `.deb`/AppImage on Linux.

### Linux/macOS shortcut

`./start.sh` wraps (b): builds the release binary if any watched source is newer, then runs it.
`./start.sh --dev` runs (a); `./start.sh --no-build` reruns the existing binary. Bash only.

## How the launcher finds the emulator

[`discover_emulator`](src-tauri/src/emulator.rs) probes in order:

1. `kyty_emulator.exe` next to the launcher binary
2. the launcher binary's parent directory
3. walking up to 8 parent directories, checking `_Build/windows/install/kyty_emulator.exe`
   at each (`_Build/linux/install/` / `_Build/macos/install/` on the other platforms)
4. `PATH`

Step 3 is what makes a dev checkout work with no configuration: from
`src-tauri/target/release/` it takes 5 levels to reach the repo root, well inside the limit.
If your emulator lives elsewhere, put it on `PATH` or set the path in Settings.

## Where settings live

The launcher reads and writes the same `Kyty.ini` as the Qt launcher, resolved by
[`resolve_settings_path`](src-tauri/src/config.rs):

1. `Kyty.ini` in the current working directory wins — portable install
2. otherwise the per-user config directory:

| OS | Path |
|---|---|
| Windows | `%APPDATA%\Kyty\Kyty.ini` |
| macOS | `~/Library/Application Support/Kyty/Kyty.ini` |
| Linux | `~/.config/Kyty/Kyty.ini` |

## Windows launch behaviour

- **Normal launch** passes `CREATE_NO_WINDOW`, so the emulator's console is hidden. Its output
  is not lost — it streams to the in-app console and the session log.
- **External terminal** launch mode opens `cmd.exe /K` in a new console instead, when you want
  a live terminal.
- **Auto-close on launch** (Settings > Emulator Settings, on by default) exits the launcher GUI
  when a game starts; a detached supervisor owns the emulator, records playtime, then relaunches
  the launcher. Turn it off to keep the window open during play.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Could not find kyty_emulator` on Launch | emulator not built or installed | run step 2 |
| `Could not connect to localhost: Connection refused` | ran the debug binary standalone | use the release binary, or `npm run tauri dev` |
| Dev server exits immediately | port 1421 taken (`strictPort`) | kill the holder, see 4a |
| UI text falls back to English | missing locale keys | `npm run i18n:coverage` reports missing/stale keys per locale |
| Rust build fails on a missing autogenerated permissions file | `src-tauri/target/` was copied from another absolute path, stale build-script cache | `Remove-Item -Recurse -Force src-tauri\target` and rebuild |
