# PS5 ShellUI Frontend for KytyPS5 (Rust)

An optional, standalone PlayStation 5-inspired dashboard and launcher frontend for `kyty_emulator`, written in pure Rust using `egui`/`eframe` and `gilrs`.

## Features

- **PS5-style Home Screen:** Horizontal spring-animated game ribbon, full-bleed 16:9 hero artwork with smooth crossfade, dynamic ambient background, boot intro, and `Games` / `Media` tabs.
- **Automatic PS5 Game Discovery:** Scans game folders for `eboot.bin`, `sce_sys/param.json`, `sce_sys/icon0.png`, `sce_sys/pic0.png`/`pic1.png`, as well as standalone `.elf` and `.zar` files.
- **Direct `kyty_emulator` CLI Integration:** Launches titles directly with configurable resolution, fullscreen, HDR10, VBlank (60/120 Hz), shader cache, performance overlay, PS5 Pro (`--trinity`) + PSSR upscaler (`Linear`/`DLSS`/`FSR`), AMD CPU patches, native RT, and DualSense vibration/volume/lightbar settings.
- **Gamepad & Keyboard Navigation:** Supports DualSense / XInput controllers via `gilrs` alongside keyboard (`Arrow keys`, `Enter`, `Q`/`E`, `Tab` Control Center, `F11` Fullscreen, `Esc`).

## Building

Requires a standard Rust toolchain (`cargo` / `rustc`):

```bash
cd tools/ps5-ui-rust
cargo build --release
```

The standalone binary is produced at `target/release/ps5-ui-rust(.exe)`.
