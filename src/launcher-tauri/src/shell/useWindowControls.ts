import { useCallback, useEffect, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";

// @tauri-apps/api/window declares this union but does not export it from
// the package's public type surface -- redeclared here to match
// Window.startResizeDragging()'s parameter exactly.
export type ResizeDirection = "East" | "North" | "NorthEast" | "NorthWest" | "South" | "SouthEast" | "SouthWest" | "West";

/** Custom-titlebar window controls, for use once `decorations: false` is set
 * in tauri.conf.json. See the plan's Risks section: `allow-start-dragging`
 * alone is NOT sufficient on Linux — the window also needs
 * `allow-internal-toggle-maximize` (double-click on the drag region),
 * `allow-toggle-maximize`, `allow-is-maximized`, `allow-minimize` and
 * `allow-close`, all declared in capabilities/default.json.
 *
 * Landed while `decorations` is still `true` (Step 2 of the rewrite plan) so
 * it can be exercised — buttons work, but drag/resize have no effect until
 * decorations are actually turned off — before the flag flips.
 */
export function useWindowControls() {
  const [maximized, setMaximized] = useState(false);

  useEffect(() => {
    const win = getCurrentWindow();
    void win.isMaximized().then(setMaximized);
    const unlisten = win.onResized(() => void win.isMaximized().then(setMaximized));
    return () => void unlisten.then((off) => off());
  }, []);

  const minimize = useCallback(() => {
    void getCurrentWindow().minimize();
  }, []);

  const toggleMaximize = useCallback(() => {
    void getCurrentWindow()
      .toggleMaximize()
      .then(() => getCurrentWindow().isMaximized())
      .then(setMaximized);
  }, []);

  const close = useCallback(() => {
    void getCurrentWindow().close();
  }, []);

  /** Attach to a spacer element's onMouseDown — never to an element that
   * also contains buttons, or the buttons start a drag instead of clicking
   * (data-tauri-drag-region fires on mousedown on Linux). */
  const startDragging = useCallback((e: React.MouseEvent) => {
    // Let clicks on real controls (buttons, links) inside the drag region
    // through without starting a window drag.
    if ((e.target as HTMLElement).closest("button,a,input,select")) return;
    void getCurrentWindow().startDragging();
  }, []);

  const startResizeDragging = useCallback((direction: ResizeDirection) => {
    void getCurrentWindow().startResizeDragging(direction);
  }, []);

  return { maximized, minimize, toggleMaximize, close, startDragging, startResizeDragging };
}
