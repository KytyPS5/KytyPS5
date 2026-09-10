// Two independent binding sets share one diagram-based remap UI:
//
// - Keyboard & mouse: captured via DOM keydown/mousedown (the webview owns
//   keyboard focus, so this is the only way to catch "press a key").
//   Produces `--keymap Id=Binding` strings, unchanged from before.
// - Physical gamepad: captured via the browser Gamepad API
//   (`navigator.getGamepads()`, polled, no "gamepad button" DOM event
//   exists), matching the W3C "standard" gamepad button/axis layout, which
//   is the same physical layout SDL_GameController (what kyty_emulator
//   actually reads) already assumes. Produces `--gamepad-map
//   Control=SdlName` strings consumed by window.cpp's GamepadRemap.
//
// The diagram (public/art/controller_diagram.png) depicts a DualSense-
// shaped silhouette at the owner's explicit request (2026-09-08, cropped
// from a reference photo, cropped to its front-view panel) -- flagged once
// as a closer trade-dress consideration than the previous generic
// wireframe, since a PS5-shaped body silhouette IS the button layout on a
// real DualSense; the decision was to proceed on that basis, same review
// posture as the kyty_mark brand asset. Still carries NO PlayStation/Sony
// branding: no logo, no button-glyph icons
// (face buttons are blank circles in the art), no wordmark. Labels use
// plain text (Triangle/Circle/Cross/Square) rather than the trademarked
// shape icons -- unchanged from the original convention.

import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import modalStyles from "./Modal.module.css";
import styles from "./InputMappingDialog.module.css";
import { Dropdown } from "./Dropdown";
import { subscribeGamepadFrames } from "../nav/gamepadSource";
import { useFocusNav } from "../nav/FocusNav";
import { t, useT } from "../i18n";

/** Names of currently connected gamepads, for the Input Device dropdown --
 * a settings-screen list, not a navigation input, so this polls at a plain
 * interval rather than useGamepadActions.ts's per-frame rAF loop.
 *
 * Reads natively (`list_gamepad_names`, lib.rs -- like Ryujinx's own SDL2
 * enumeration) rather than through the browser Gamepad API, and prefers
 * that result whenever it has anything: for privacy, `navigator.
 * getGamepads()` reports a pad's presence only after the user has pressed
 * one of its buttons at least once, so a controller that is plugged in but
 * untouched is invisible to it -- confirmed on this exact setup (a
 * connected Xbox controller stayed unnamed until first pressed). The
 * browser API is kept as a fallback for platforms `list_gamepad_names`
 * does not cover (native support is Linux-only today, see lib.rs) and for
 * the webview's own actual input *capture*, which still goes through
 * GamepadCaptureOverlay's Gamepad API polling unchanged -- this hook only
 * feeds the dropdown's display names. */
function useConnectedGamepadNames(): string[] {
  const [nativeNames, setNativeNames] = useState<string[]>([]);
  const [browserNames, setBrowserNames] = useState<string[]>([]);

  useEffect(() => {
    let cancelled = false;
    const poll = () => {
      void invoke<string[]>("list_gamepad_names")
        .then((names) => {
          if (!cancelled) setNativeNames(names);
        })
        .catch(() => undefined);
    };
    poll();
    const interval = setInterval(poll, 1000);
    return () => {
      cancelled = true;
      clearInterval(interval);
    };
  }, []);

  useEffect(() => {
    if (!("getGamepads" in navigator)) return;
    const poll = () => {
      const next = Array.from(navigator.getGamepads())
        .filter((pad): pad is Gamepad => pad !== null)
        .map((pad) => pad.id);
      setBrowserNames((prev) => (prev.length === next.length && prev.every((n, i) => n === next[i]) ? prev : next));
    };
    poll();
    const interval = setInterval(poll, 500);
    window.addEventListener("gamepadconnected", poll);
    window.addEventListener("gamepaddisconnected", poll);
    return () => {
      clearInterval(interval);
      window.removeEventListener("gamepadconnected", poll);
      window.removeEventListener("gamepaddisconnected", poll);
    };
  }, []);

  return nativeNames.length > 0 ? nativeNames : browserNames;
}

interface Target {
  id: string;
  /** i18n key under input.label.* -- resolved at render, not stored
   * translated, since these arrays are module-level constants. */
  labelKey: string;
  defaultBinding: string;
  side: "left" | "right";
}

interface GamepadTarget extends Target {
  kind: "button" | "stickAxis" | "trigger";
}

const KEYBOARD_TARGETS: Target[] = [
  { id: "L2", labelKey: "input.label.l2", defaultBinding: "", side: "left" },
  { id: "L1", labelKey: "input.label.l1", defaultBinding: "Q", side: "left" },
  { id: "Up", labelKey: "input.label.dpadUp", defaultBinding: "Up", side: "left" },
  { id: "Left", labelKey: "input.label.dpadLeft", defaultBinding: "Left", side: "left" },
  { id: "Down", labelKey: "input.label.dpadDown", defaultBinding: "Down", side: "left" },
  { id: "Right", labelKey: "input.label.dpadRight", defaultBinding: "Right", side: "left" },
  { id: "L3", labelKey: "input.label.l3", defaultBinding: "Left Shift", side: "left" },
  { id: "LeftStickUp", labelKey: "input.label.leftStickUp", defaultBinding: "W", side: "left" },
  { id: "LeftStickDown", labelKey: "input.label.leftStickDown", defaultBinding: "S", side: "left" },
  { id: "LeftStickLeft", labelKey: "input.label.leftStickLeft", defaultBinding: "A", side: "left" },
  { id: "LeftStickRight", labelKey: "input.label.leftStickRight", defaultBinding: "D", side: "left" },

  { id: "R2", labelKey: "input.label.r2", defaultBinding: "", side: "right" },
  { id: "R1", labelKey: "input.label.r1", defaultBinding: "E", side: "right" },
  { id: "Triangle", labelKey: "input.label.triangle", defaultBinding: "I", side: "right" },
  { id: "Circle", labelKey: "input.label.circle", defaultBinding: "L", side: "right" },
  { id: "Cross", labelKey: "input.label.cross", defaultBinding: "J", side: "right" },
  { id: "Square", labelKey: "input.label.square", defaultBinding: "K", side: "right" },
  { id: "Options", labelKey: "input.label.options", defaultBinding: "Return", side: "right" },
  { id: "TouchPad", labelKey: "input.label.touchPad", defaultBinding: "Backspace", side: "right" },
  { id: "R3", labelKey: "input.label.r3", defaultBinding: "Left Ctrl", side: "right" },
  { id: "RightStickUp", labelKey: "input.label.rightStickUp", defaultBinding: "T", side: "right" },
  { id: "RightStickDown", labelKey: "input.label.rightStickDown", defaultBinding: "G", side: "right" },
  { id: "RightStickLeft", labelKey: "input.label.rightStickLeft", defaultBinding: "F", side: "right" },
  { id: "RightStickRight", labelKey: "input.label.rightStickRight", defaultBinding: "H", side: "right" },
];

// No default bindings: an empty gamepad_keymap means "use kyty_emulator's
// built-in Xbox-layout defaults" (see window.cpp's GamepadRemap), so there
// is nothing to pre-fill here the way the keyboard defaults are.
const GAMEPAD_TARGETS: GamepadTarget[] = [
  { id: "L1", labelKey: "input.label.l1", defaultBinding: "", side: "left", kind: "button" },
  { id: "TriggerLeft", labelKey: "input.label.l2", defaultBinding: "", side: "left", kind: "trigger" },
  { id: "Up", labelKey: "input.label.dpadUp", defaultBinding: "", side: "left", kind: "button" },
  { id: "Left", labelKey: "input.label.dpadLeft", defaultBinding: "", side: "left", kind: "button" },
  { id: "Down", labelKey: "input.label.dpadDown", defaultBinding: "", side: "left", kind: "button" },
  { id: "Right", labelKey: "input.label.dpadRight", defaultBinding: "", side: "left", kind: "button" },
  { id: "L3", labelKey: "input.label.l3", defaultBinding: "", side: "left", kind: "button" },
  { id: "LeftStickX", labelKey: "input.label.leftStickX", defaultBinding: "", side: "left", kind: "stickAxis" },
  { id: "LeftStickY", labelKey: "input.label.leftStickY", defaultBinding: "", side: "left", kind: "stickAxis" },

  { id: "R1", labelKey: "input.label.r1", defaultBinding: "", side: "right", kind: "button" },
  { id: "TriggerRight", labelKey: "input.label.r2", defaultBinding: "", side: "right", kind: "trigger" },
  { id: "Triangle", labelKey: "input.label.triangle", defaultBinding: "", side: "right", kind: "button" },
  { id: "Circle", labelKey: "input.label.circle", defaultBinding: "", side: "right", kind: "button" },
  { id: "Cross", labelKey: "input.label.cross", defaultBinding: "", side: "right", kind: "button" },
  { id: "Square", labelKey: "input.label.square", defaultBinding: "", side: "right", kind: "button" },
  { id: "Options", labelKey: "input.label.options", defaultBinding: "", side: "right", kind: "button" },
  { id: "TouchPad", labelKey: "input.label.touchPad", defaultBinding: "", side: "right", kind: "button" },
  { id: "R3", labelKey: "input.label.r3", defaultBinding: "", side: "right", kind: "button" },
  { id: "RightStickX", labelKey: "input.label.rightStickX", defaultBinding: "", side: "right", kind: "stickAxis" },
  { id: "RightStickY", labelKey: "input.label.rightStickY", defaultBinding: "", side: "right", kind: "stickAxis" },
];

// W3C "Standard Gamepad" button index -> SDL_GameController button name.
// Indices 6/7 (LT/RT) are handled separately via .value, not here.
const GAMEPAD_BUTTON_NAMES: Record<number, string> = {
  0: "a", 1: "b", 2: "x", 3: "y",
  4: "leftshoulder", 5: "rightshoulder",
  8: "back", 9: "start",
  10: "leftstick", 11: "rightstick",
  12: "dpup", 13: "dpdown", 14: "dpleft", 15: "dpright",
};
const GAMEPAD_AXIS_NAMES: Record<number, string> = { 0: "leftx", 1: "lefty", 2: "rightx", 3: "righty" };

function keyName(e: KeyboardEvent): string {
  const key = e.key;
  if (key.length === 1) return key.toUpperCase();
  const named: Record<string, string> = {
    Enter: "Return", Backspace: "Backspace", Tab: "Tab", Shift: "Left Shift",
    Control: "Left Ctrl", Alt: "Left Alt", Meta: "Left GUI", Insert: "Insert",
    Delete: "Delete", Home: "Home", End: "End", PageUp: "PageUp", PageDown: "PageDown",
    ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down",
    CapsLock: "CapsLock", NumLock: "Numlock", ScrollLock: "ScrollLock",
    Pause: "Pause", PrintScreen: "PrintScreen",
  };
  if (named[key]) return named[key];
  if (/^F\d{1,2}$/.test(key)) return key;
  return "";
}

function parseMapping(mapping: string[]): Record<string, string> {
  const result: Record<string, string> = {};
  for (const entry of mapping) {
    const sep = entry.indexOf("=");
    if (sep > 0 && sep + 1 < entry.length) result[entry.slice(0, sep)] = entry.slice(sep + 1);
  }
  return result;
}

function KeyboardCaptureOverlay({ onCapture, onCancel }: { onCapture: (binding: string) => void; onCancel: () => void }) {
  const [message, setMessage] = useState(t("input.captureKeyboardPrompt"));

  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      e.preventDefault();
      if (e.repeat) return;
      if (e.key === "Escape") return onCancel();
      if (e.key === " " || e.key === "F1") return setMessage(t("input.captureReservedKey"));
      const name = keyName(e);
      if (name) onCapture(name);
      else setMessage(t("input.captureUnsupportedKey"));
    };
    const onMouseDown = (e: MouseEvent) => {
      e.preventDefault();
      const name = ["Mouse:Left", "Mouse:Middle", "Mouse:Right", "Mouse:X1", "Mouse:X2"][e.button];
      if (name) onCapture(name);
    };
    window.addEventListener("keydown", onKeyDown, true);
    window.addEventListener("mousedown", onMouseDown, true);
    return () => {
      window.removeEventListener("keydown", onKeyDown, true);
      window.removeEventListener("mousedown", onMouseDown, true);
    };
  }, [onCapture, onCancel]);

  return <CaptureOverlayFrame message={message} onCancel={onCancel} />;
}

/** Checks one pad (native or browser -- same {pressed,value}/axes[] shape)
 * for the specific input targetKind is waiting on. Returns the SDL name to
 * capture, or null if nothing matching is currently active on this pad. */
function captureFromPad(
  pad: { buttons: readonly { pressed: boolean; value: number }[]; axes: readonly number[] },
  targetKind: GamepadTarget["kind"],
): string | null {
  if (targetKind === "button") {
    for (const [index, name] of Object.entries(GAMEPAD_BUTTON_NAMES)) {
      if (pad.buttons[Number(index)]?.pressed) return name;
    }
  } else if (targetKind === "trigger") {
    if ((pad.buttons[6]?.value ?? 0) > 0.5) return "lefttrigger";
    if ((pad.buttons[7]?.value ?? 0) > 0.5) return "righttrigger";
  } else {
    for (const [index, name] of Object.entries(GAMEPAD_AXIS_NAMES)) {
      if (Math.abs(pad.axes[Number(index)] ?? 0) > 0.6) return name;
    }
  }
  return null;
}

function GamepadCaptureOverlay({
  targetKind,
  onCapture,
  onCancel,
}: {
  targetKind: GamepadTarget["kind"];
  onCapture: (binding: string) => void;
  onCancel: () => void;
}) {
  const [message, setMessage] = useState(
    targetKind === "button" ? t("input.captureGamepadButtonPrompt") : t("input.captureGamepadAxisPrompt"),
  );

  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === "Escape") onCancel();
    };
    window.addEventListener("keydown", onKeyDown, true);

    let sawAnyPad = false;
    let captured = false;

    // Shared frame source (nav/gamepadSource.ts), not this overlay's own
    // `poll_gamepad_state` RAF loop -- useGamepadActions.ts subscribes to
    // the same one, so a controller drives both navigation and this capture
    // dialog off a single IPC poll per frame rather than two competing ones.
    const unsubscribe = subscribeGamepadFrames((frame) => {
      if (captured) return;

      for (const pad of frame.native) {
        sawAnyPad = true;
        const name = captureFromPad(pad, targetKind);
        if (name) {
          captured = true;
          onCapture(name);
          return;
        }
      }
      if ("getGamepads" in navigator) {
        for (const pad of frame.browser) {
          sawAnyPad = true;
          const name = captureFromPad({ buttons: pad.buttons, axes: pad.axes }, targetKind);
          if (name) {
            captured = true;
            onCapture(name);
            return;
          }
        }
      } else if (frame.native.length === 0 && !sawAnyPad) {
        // Neither source is available at all on this platform/build.
        setMessage(t("input.noGamepadApi"));
      }
    });

    return () => {
      window.removeEventListener("keydown", onKeyDown, true);
      unsubscribe();
    };
  }, [targetKind, onCapture, onCancel]);

  return <CaptureOverlayFrame message={message} onCancel={onCancel} />;
}

function CaptureOverlayFrame({ message, onCancel }: { message: string; onCancel: () => void }) {
  const rootRef = useRef<HTMLDivElement>(null);
  const { pushFocusScope, popFocusScope } = useFocusNav();

  // Both capture overlays render through this frame. Neither pushed a
  // focus scope before, so the d-pad kept moving .ps-focused around
  // whatever was behind the overlay, and a gamepad Circle press arrived as
  // FocusNav's "back" and routed to the enclosing scope instead of this
  // overlay's own onCancel. Same pushFocusScope/popFocusScope contract
  // Modal.tsx already uses.
  useEffect(() => {
    if (!rootRef.current) return;
    pushFocusScope(rootRef.current, onCancel);
    return () => popFocusScope();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <div
      ref={rootRef}
      onClick={onCancel}
      style={{ position: "fixed", inset: 0, zIndex: 300, background: "var(--backdrop-scrim)", backdropFilter: "blur(6px)", display: "flex", alignItems: "center", justifyContent: "center" }}
    >
      <div className={modalStyles.surface} style={{ padding: "28px 36px", textAlign: "center", maxWidth: 360 }}>
        <p style={{ margin: 0, fontSize: 13, color: "var(--text-secondary)" }}>{message}</p>
      </div>
    </div>
  );
}

/** The reference DualSense-shaped diagram (see this file's header comment
 * for provenance). Purely illustrative (no per-control leader lines -- see
 * RemapPanel). A plain <img>, no live filter -- a static raster paint costs
 * nothing per frame (same rule as theme.css's film grain).
 *
 * Deliberately not an SVG <image> (confirmed real bug, 2026-09-09): this
 * WebKitGTK build paints an opaque rectangle across the full <image>
 * bounding box instead of respecting the PNG's alpha channel whenever it is
 * referenced from inside an <svg>, verified by isolating the transparent
 * PNG in an ordinary <img> (renders correctly, no box) versus the identical
 * PNG in an <svg><image> (the box appears regardless of letterboxing). A
 * plain <img> sized off its own 1053:640 intrinsic aspect (width: 100%,
 * height: auto) reproduces the same fit without going through SVG's image
 * compositing path at all -- see RemapPanel's diagram container for why
 * width-driven sizing, not object-fit, is what keeps this from inflating
 * the row. */
function ControllerDiagram() {
  return (
    <img
      src="/art/controller_diagram.png"
      alt=""
      style={{ display: "block", width: "100%", height: "auto" }}
    />
  );
}

function RemapPanel({
  targets,
  bindings,
  selected,
  onActivate,
}: {
  targets: Target[];
  bindings: Record<string, string>;
  selected: string;
  onActivate: (id: string) => void;
}) {
  const left = targets.filter((c) => c.side === "left");
  const right = targets.filter((c) => c.side === "right");

  const renderLabel = (c: Target) => {
    const active = selected === c.id;
    const binding = bindings[c.id];
    return (
      <button
        key={c.id}
        type="button"
        data-focusable
        onClick={() => onActivate(c.id)}
        className={active ? styles.chipSelected : styles.chip}
      >
        <span className={styles.chipLabel}>{t(c.labelKey)}</span>
        <span className={binding ? styles.chipValue : styles.chipValueEmpty}>{binding || t("common.none")}</span>
      </button>
    );
  };

  return (
    <div style={{ display: "flex", alignItems: "stretch", justifyContent: "flex-start", gap: 16 }}>
      <div style={{ display: "flex", flexDirection: "column", justifyContent: "center", gap: 6 }}>{left.map(renderLabel)}</div>

      {/* Width-capped at 504px (renders ~306px tall off the art's 1053:640
         aspect, 10% down from the previous 560px cap) so GamepadCategory's
         .gamepadPane -- sized to exactly this row's width, see
         Settings.module.css -- can't inflate this further. `stretch` on the
         row above + `center` here keeps the art vertically centered against
         whichever chip column is taller, rather than pinned to the shorter
         one's top.

         Purely illustrative (owner decision, 2026-09-08): no per-control
         leader lines to the art -- the label lists on either side are the
         actual interactive UI. The radial-gradient background is a static
         paint, not a live filter -- same "cheap ambience, no per-frame
         cost" rule as theme.css's film grain -- so it's safe on a static
         modal backdrop. */}
      <div
        style={{
          flex: "0 1 504px",
          maxWidth: 504,
          display: "flex",
          alignItems: "center",
          borderRadius: "var(--radius-lg)",
          background: "radial-gradient(ellipse at 50% 15%, rgba(255,255,255,0.07) 0%, transparent 62%)",
        }}
      >
        <ControllerDiagram />
      </div>

      <div style={{ display: "flex", flexDirection: "column", justifyContent: "center", gap: 6 }}>{right.map(renderLabel)}</div>
    </div>
  );
}

type Mode = "keyboard" | "gamepad";

export interface RemapResult {
  hostInputMapping: string[];
  gamepadKeymap: string[];
}

/** The actual remap UI (mode toggle, diagram, capture) -- self-contained so
 * it can be embedded two ways: inside a Modal with explicit Save/Cancel
 * (InputMappingDialog, below -- Library's flow), or directly inline in a
 * page with changes applying live (Settings' Gamepad category -- no
 * "Configure gamepad buttons…" detour needed there). `onChange` fires after
 * every capture/clear/restore-defaults; callers that want an explicit save
 * step buffer it themselves instead of applying it immediately. */
export function ControllerRemapEditor({
  mapping,
  gamepadMapping,
  onChange,
}: {
  mapping: string[];
  gamepadMapping: string[];
  onChange: (result: RemapResult) => void;
}) {
  const t = useT();
  const gamepadNames = useConnectedGamepadNames();
  // One Input Device dropdown drives everything (Ryujinx's model), rather
  // than a separate "Keyboard & mouse"/"Gamepad" button pair: it also shows
  // every gamepad actually detected by name, not just a generic "Gamepad"
  // label. There is still only one gamepad remap table regardless of which
  // physical device is connected (kyty_emulator's SDL2 layer handles the
  // per-device layout already -- see this file's header comment), so
  // picking between two connected gamepads here does not change behavior,
  // it only reflects what is plugged in.
  const inputDeviceOptions: { value: string; label: string }[] = [
    { value: "keyboard", label: t("input.keyboardMouse") },
    ...(gamepadNames.length > 0
      ? gamepadNames.map((name, i) => ({ value: `gamepad:${i}`, label: name }))
      // Nothing detected yet is very often "connected but never pressed" (see
      // useConnectedGamepadNames's doc comment), not "nothing connected" --
      // say so rather than implying detection failed.
      : [{ value: "gamepad", label: t("input.gamepadNotDetectedYet") }]),
  ];
  const [device, setDevice] = useState<string>("gamepad");
  const deviceValue = inputDeviceOptions.some((o) => o.value === device)
    ? device
    : device === "keyboard"
      ? "keyboard"
      : inputDeviceOptions[1].value;
  const mode: Mode = deviceValue === "keyboard" ? "keyboard" : "gamepad";

  const kbCustomized = mapping.length > 0;
  const [kbBindings, setKbBindings] = useState<Record<string, string>>(() =>
    kbCustomized
      ? parseMapping(mapping)
      : Object.fromEntries(KEYBOARD_TARGETS.map((c) => [c.id, c.defaultBinding])),
  );
  const [kbWasCustomized, setKbWasCustomized] = useState(kbCustomized);
  const [kbSelected, setKbSelected] = useState(KEYBOARD_TARGETS[0].id);

  const [gpBindings, setGpBindings] = useState<Record<string, string>>(() => parseMapping(gamepadMapping));
  const [gpSelected, setGpSelected] = useState(GAMEPAD_TARGETS[0].id);

  const [captureFor, setCaptureFor] = useState<string | null>(null);

  const emit = (kb: Record<string, string>, kbCustom: boolean, gp: Record<string, string>) => {
    onChange({
      hostInputMapping: kbCustom
        ? KEYBOARD_TARGETS.filter((c) => kb[c.id]).map((c) => `${c.id}=${kb[c.id]}`)
        : [],
      gamepadKeymap: GAMEPAD_TARGETS.filter((c) => gp[c.id]).map((c) => `${c.id}=${gp[c.id]}`),
    });
  };

  const setKbBinding = (id: string, binding: string) => {
    setKbBindings((prev) => {
      const next = { ...prev };
      for (const key of Object.keys(next)) {
        if (key !== id && binding !== "" && next[key].toLowerCase() === binding.toLowerCase()) next[key] = "";
      }
      next[id] = binding;
      emit(next, true, gpBindings);
      return next;
    });
    setKbWasCustomized(true);
  };

  const setGpBinding = (id: string, binding: string) => {
    setGpBindings((prev) => {
      const next = { ...prev };
      for (const key of Object.keys(next)) {
        if (key !== id && binding !== "" && next[key] === binding) next[key] = "";
      }
      next[id] = binding;
      emit(kbBindings, kbWasCustomized, next);
      return next;
    });
  };

  const restoreDefaults = () => {
    if (mode === "keyboard") {
      const next = Object.fromEntries(KEYBOARD_TARGETS.map((c) => [c.id, c.defaultBinding]));
      setKbBindings(next);
      setKbWasCustomized(false);
      emit(next, false, gpBindings);
    } else {
      setGpBindings({});
      emit(kbBindings, kbWasCustomized, {});
    }
  };

  const gamepadTarget = GAMEPAD_TARGETS.find((t) => t.id === captureFor);
  const currentBinding = mode === "keyboard" ? kbBindings[kbSelected] : gpBindings[gpSelected];

  return (
    <div>
      <div style={{ display: "flex", alignItems: "center", gap: 12, marginBottom: 10 }}>
        <span style={{ fontSize: 12, fontWeight: 600, color: "var(--text-secondary)" }}>{t("input.inputDevice")}</span>
        <div style={{ minWidth: 260 }}>
          <Dropdown ariaLabel={t("input.inputDevice")} value={deviceValue} onChange={setDevice} options={inputDeviceOptions} />
        </div>
        <div style={{ display: "flex", gap: 10, marginLeft: "auto" }}>
          <button
            className="pill-button"
            disabled={!currentBinding}
            onClick={() =>
              mode === "keyboard" ? setKbBinding(kbSelected, "") : setGpBinding(gpSelected, "")
            }
          >
            {t("common.clear")}
          </button>
          <button className="pill-button" onClick={restoreDefaults}>
            {t("common.defaults")}
          </button>
        </div>
      </div>

      <p style={{ margin: "0 0 16px", fontSize: 12, color: "var(--text-muted)" }}>
        {mode === "keyboard" ? t("input.helpKeyboard") : t("input.helpGamepad")}
      </p>

      {/* Selecting a target now selects AND immediately starts capture in
         one action (no separate "Change..." button): the plain <div>
         labels this replaced were mouse-only (onClick to select,
         onDoubleClick to capture), unreachable by FocusNav at all --
         data-focusable real <button>s here are what actually makes this
         screen usable with a gamepad or keyboard alone. */}
      {mode === "keyboard" ? (
        <RemapPanel
          targets={KEYBOARD_TARGETS}
          bindings={kbBindings}
          selected={kbSelected}
          onActivate={(id) => {
            setKbSelected(id);
            setCaptureFor(id);
          }}
        />
      ) : (
        <RemapPanel
          targets={GAMEPAD_TARGETS}
          bindings={gpBindings}
          selected={gpSelected}
          onActivate={(id) => {
            setGpSelected(id);
            setCaptureFor(id);
          }}
        />
      )}

      {captureFor && mode === "keyboard" && (
        <KeyboardCaptureOverlay
          onCancel={() => setCaptureFor(null)}
          onCapture={(binding) => {
            setKbBinding(captureFor, binding);
            setCaptureFor(null);
          }}
        />
      )}
      {captureFor && mode === "gamepad" && gamepadTarget && (
        <GamepadCaptureOverlay
          targetKind={gamepadTarget.kind}
          onCancel={() => setCaptureFor(null)}
          onCapture={(binding) => {
            setGpBinding(captureFor, binding);
            setCaptureFor(null);
          }}
        />
      )}
    </div>
  );
}
