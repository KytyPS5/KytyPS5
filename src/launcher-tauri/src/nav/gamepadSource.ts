import { pollNativeGamepads, type NativeGamepadState } from "./nativeGamepad";

/** One shared per-frame gamepad snapshot, so navigation's two consumers
 * (useGamepadActions' button/d-pad/stick polling, previously useStickPointer
 * too) don't each run their own `requestAnimationFrame` loop issuing their
 * own `invoke("poll_gamepad_state")` -- that was 2 IPC round trips per frame
 * for the same data, confirmed while investigating the stick-nav "not
 * fluid" report. A single module-level loop starts on first subscriber and
 * stops when the last one unsubscribes, publishing merged native + browser
 * Gamepad API pads to everyone on the same frame. */

export interface GamepadFrame {
  native: NativeGamepadState[];
  browser: Gamepad[];
}

type Listener = (frame: GamepadFrame) => void;

const listeners = new Set<Listener>();
let raf = 0;
let running = false;

function loop() {
  void pollNativeGamepads().then((native) => {
    if (!running) return;
    const browser: Gamepad[] = "getGamepads" in navigator ? Array.from(navigator.getGamepads()).filter((p): p is Gamepad => p !== null) : [];
    const frame: GamepadFrame = { native, browser };
    for (const l of listeners) l(frame);
    if (running) raf = requestAnimationFrame(loop);
  });
}

export function subscribeGamepadFrames(listener: Listener): () => void {
  listeners.add(listener);
  if (!running) {
    running = true;
    raf = requestAnimationFrame(loop);
  }
  return () => {
    listeners.delete(listener);
    if (listeners.size === 0) {
      running = false;
      cancelAnimationFrame(raf);
    }
  };
}
