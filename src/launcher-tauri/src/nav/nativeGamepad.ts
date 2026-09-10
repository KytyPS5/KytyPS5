import { invoke } from "@tauri-apps/api/core";

/** Mirrors lib.rs's NativeGamepadState/NativeButtonState -- deliberately
 * shaped like a browser GamepadButton/Gamepad (`{pressed, value}`,
 * `axes: number[]`) so code written against the Gamepad API needed only an
 * input-source swap, not a rewrite, to move to this. See lib.rs's
 * poll_gamepad_state doc comment for why this exists at all: WebKitGTK's
 * Gamepad API needs libmanette (not installed on every system) and even
 * where present will not report a connected-but-untouched controller until
 * its first button press -- both confirmed failures on this exact machine
 * with a real, working Xbox controller. */
export interface NativeButtonState {
  pressed: boolean;
  value: number;
}

export interface NativeGamepadState {
  name: string;
  /** Length 17, W3C Standard Gamepad button order -- see lib.rs. */
  buttons: NativeButtonState[];
  /** Length 4: leftX, leftY, rightX, rightY. */
  axes: number[];
}

/** Never throws -- callers already need to treat "no gamepads" as a normal
 * state (nothing connected, or gilrs failed to initialize on this system),
 * not an error to handle separately from that. */
export async function pollNativeGamepads(): Promise<NativeGamepadState[]> {
  try {
    return await invoke<NativeGamepadState[]>("poll_gamepad_state");
  } catch {
    return [];
  }
}
