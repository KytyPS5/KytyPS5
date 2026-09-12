/** Suppresses the webview's native right-click menu.
 *
 * This is a console UI driven by a gamepad and a focus ring, not a web page:
 * the browser context menu ("Back", "Reload", "Save image as...") exposes
 * navigation this app has no concept of, and offers to save the cover art
 * out of a launcher. Nothing in the UI is discoverable through it.
 *
 * Two deliberate exceptions:
 *
 * - Editable fields keep their menu. Settings is full of real text inputs
 *   (game directories, emulator path), and right-click paste is the normal
 *   way to get a long Windows path into one. Killing that would cost more
 *   than the menu does.
 * - Dev builds keep it, so "Inspect" still works while running
 *   `npm run tauri dev`. Release builds have no devtools to reach anyway.
 */

const EDITABLE = "input, textarea, [contenteditable]:not([contenteditable='false'])";

export function installContextMenuSuppression(): void {
  if (import.meta.env.DEV) return;

  window.addEventListener("contextmenu", (event) => {
    const target = event.target;
    if (target instanceof Element && target.closest(EDITABLE)) return;
    event.preventDefault();
  });
}
