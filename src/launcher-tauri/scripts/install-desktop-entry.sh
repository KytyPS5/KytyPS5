#!/usr/bin/env bash
# Installs the Kyty Launcher .desktop entry + icon for the current user, so
# GNOME/KDE on Wayland can resolve the running window's app_id
# ("kyty-launcher", the Cargo binary name -- see src-tauri/lib.rs) to a real
# icon instead of falling back to the generic gear placeholder. Wayland
# ignores window.set_icon() entirely; this desktop-entry + hicolor-icon pair
# is the actual fix (see the plan this script implements).
#
# Safe to re-run any time the icon or binary path changes.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

DESKTOP_SRC="src-tauri/kyty-launcher.desktop"
DESKTOP_DEST="$HOME/.local/share/applications/kyty-launcher.desktop"
ICON_DIR="$HOME/.local/share/icons/hicolor"

# Deliberately NEVER target/debug: that binary is produced by `tauri dev`
# with tauri.conf.json's devUrl (http://localhost:1421) baked in, so it only
# runs while `npm run tauri dev`'s Vite server is up. Launched standalone
# (from the app grid, `gtk-launch`, a .desktop double-click) it fails with
# "Could not connect to localhost: Connection refused" -- confirmed real
# failure, not a hypothetical. target/release (from `tauri build` /
# `cargo tauri build`) embeds the built frontend (frontendDist: ../dist)
# instead and runs standalone.
if [[ -x "src-tauri/target/release/kyty-launcher" ]]; then
  BIN="$(readlink -f src-tauri/target/release/kyty-launcher)"
elif command -v kyty-launcher >/dev/null 2>&1; then
  BIN="$(command -v kyty-launcher)"
else
  echo "error: no release kyty-launcher binary found (src-tauri/target/release/kyty-launcher)." >&2
  echo "       Build one first: npm run tauri build -- --no-bundle" >&2
  echo "       (or a full 'npm run tauri build' for the deb/AppImage too)." >&2
  exit 1
fi

mkdir -p "$(dirname "$DESKTOP_DEST")"
sed "s|@EXEC@|$BIN|" "$DESKTOP_SRC" > "$DESKTOP_DEST"
echo "installed $DESKTOP_DEST (Exec=$BIN)"

declare -A SIZES=(
  [32]=src-tauri/icons/32x32.png
  [64]=src-tauri/icons/64x64.png
  [128]=src-tauri/icons/128x128.png
  [256]=src-tauri/icons/128x128@2x.png
  [512]=src-tauri/icons/icon.png
)
for size in "${!SIZES[@]}"; do
  dest_dir="$ICON_DIR/${size}x${size}/apps"
  mkdir -p "$dest_dir"
  cp "${SIZES[$size]}" "$dest_dir/kyty-launcher.png"
  echo "installed $dest_dir/kyty-launcher.png"
done

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$HOME/.local/share/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f "$ICON_DIR" 2>/dev/null || true
fi

echo "done. If the dock icon still shows the old placeholder, log out/in" \
     "(GNOME Shell caches running-app icons for the session)."
