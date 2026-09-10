import { useEffect, useRef } from "react";
import { subscribeNavIntent, type NavIntent } from "./inputBus";

/** Actions FocusNav consumes. Deliberately English, not Sony glyph names --
 * "confirm"/"back"/"menu", never "cross"/"circle"/"triangle". Sourced from
 * NavIntent (inputBus.ts) rather than declared independently, so this type
 * cannot drift from what src-tauri/src/gamepad.rs actually emits. */
export type FocusAction = NavIntent["action"];
export type { NavIntent };

/** Subscribes `onIntent` to the navigation intent stream. Deadzone,
 * direction resolution and repeat timing all happen once, upstream, in the
 * Rust thread that produces these events (src-tauri/src/gamepad.rs) -- this
 * hook has no state machine of its own.
 *
 * Delivery is a direct, synchronous callback, not a React state value: the
 * previous implementation delivered events via `useState`, which meant two
 * intents arriving close enough together to land in the same React batch
 * silently collapsed into one (only the later `setState` call's value
 * survived). Calling `onIntent` straight from the subscription cannot lose
 * an event that way -- there is nothing for two calls to collapse into.
 *
 * `onIntent` is captured through a ref so passing a fresh inline callback
 * every render does not tear down and recreate the subscription. */
export function useGamepadActions(onIntent: (intent: NavIntent) => void): void {
  const onIntentRef = useRef(onIntent);
  onIntentRef.current = onIntent;
  useEffect(() => subscribeNavIntent((intent) => onIntentRef.current(intent)), []);
}
