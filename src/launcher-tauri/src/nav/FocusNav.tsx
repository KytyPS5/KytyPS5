import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from "react";
import { useGamepadActions, type FocusAction } from "./useGamepadActions";
import { subscribeNavScroll, type NavIntent, type NavScroll } from "./inputBus";
import { scrollIntoViewWithin, scrollBy as scrollRegionBy, isAnimating as isRegionAnimating } from "./scrollController";
import { playSfx } from "../lib/sfx";

/* Ported from PlayStation-Revamp's src/Components/ControllerNavigation.jsx.
 * The valuable part is findNearest's scoring: a candidate must be >3px past
 * the current center on the primary axis, and the winner minimizes
 * primaryDist + crossDist*3 -- that 3x cross-axis penalty is what makes
 * traversal feel console-like instead of arbitrary. Ported unchanged.
 *
 * Differences from the source, all deliberate (see the rewrite plan's
 * Part 5):
 *  - Input is useGamepadActions() (local Gamepad API polling) instead of
 *    useDualSenseWS() (phone-over-websocket) -- this launcher has no phone
 *    pairing feature.
 *  - No react-router: "back" maps to a caller-supplied onBack callback
 *    (close a modal, else return to Library) instead of navigate(-1), and
 *    the reference's per-route focus-clear-on-location-change instead
 *    clears whenever the caller's `resetKey` prop changes (pass the active
 *    ViewId).
 *  - Action names are confirm/back/menu, never cross/circle/triangle --
 *    see useGamepadActions.ts's FocusAction doc.
 *  - No window.__focusNavActive / controller-focus-move wiring: the
 *    reference's VirtualCursor was a rendered DOM cursor element position-
 *    synced from outside React, driving focus by hit-testing wherever it
 *    landed. This app tried that (useStickPointer) and retired it (owner
 *    decision 2026-09-09): the left stick is now discrete d-pad-style nav,
 *    same as the physical d-pad, feeding useGamepadActions' up/down/left/
 *    right actions below instead of a second, continuous input model. The
 *    real OS cursor is still hidden via CSS whenever gamepad input is the
 *    one driving -- see setPointerHidden.
 */

// input:not([type="checkbox"]):not([type="radio"]) is deliberate: a raw
// text/number <input> (ConfigForm's resolution/vblank/log-path fields) had
// no way to be reached by spatial nav at all before this -- gamepad
// "confirm" on one calls el.click(), which natively focuses it for a
// physical keyboard to type into, same as clicking it with a mouse already
// did. Checkbox/radio inputs are excluded: this app's on/off controls put
// data-focusable on the wrapping <label> instead (PatchesPane's checkbox
// rows; components/Toggle.tsx's synthetic switch, which has no underlying
// <input> at all), so confirming the row toggles it without a second,
// visually-redundant focusable target landing on the checkbox itself.
const FOCUSABLE_SELECTOR = [
  "a[href]:not([data-no-focus])",
  "button:not([disabled]):not([data-no-focus])",
  '[role="button"]:not([data-no-focus])',
  'input:not([disabled]):not([type="checkbox"]):not([type="radio"]):not([type="hidden"]):not([data-no-focus])',
  "[data-focusable]",
].join(", ");

type Direction = "up" | "down" | "left" | "right";

/** Resolves which `[data-scroll-region]` a boundary-scroll should act on.
 * The fast path (`closest`) covers the common case: the focused element is
 * a descendant of the thing that scrolls (Library's grid, Settings' rail).
 * It misses when focus sits in a sidebar whose *sibling* panel is what
 * actually needs to scroll (GameDetail's `.rail` buttons vs. its `.detail`
 * pane, which is where Trophies/Logs/patches live) -- `closest` only walks
 * ancestors, and `.detail` is not one. The fallback searches the current
 * scope's root (or `document` with none open) for scroll regions and picks
 * whichever is geometrically closest to the focused element, so this still
 * resolves correctly if a scope ever holds more than one candidate. */
function findScrollHost(focusedEl: HTMLElement, scopeRoot: ParentNode): HTMLElement | null {
  const direct = focusedEl.closest<HTMLElement>("[data-scroll-region]");
  if (direct) return direct;
  const candidates = Array.from(scopeRoot.querySelectorAll<HTMLElement>("[data-scroll-region]"));
  if (candidates.length === 0) return null;
  if (candidates.length === 1) return candidates[0];
  const fr = focusedEl.getBoundingClientRect();
  const fy = fr.top + fr.height / 2;
  let best = candidates[0];
  let bestDist = Infinity;
  for (const c of candidates) {
    const cr = c.getBoundingClientRect();
    const dist = Math.abs(cr.top + cr.height / 2 - fy);
    if (dist < bestDist) {
      bestDist = dist;
      best = c;
    }
  }
  return best;
}

/** One entry per open overlay/modal. `restoreEl` is whatever had focus right
 * before this scope opened (05-acceptance-checklist's "returning from an
 * overlay restores previous focus"); `onEscape` is the owning component's
 * own close callback (typically a `setOpen(false)`) -- FocusNav does not
 * know how to visually close an overlay, only how to route Escape/back to
 * whoever does. */
interface FocusScopeEntry {
  root: HTMLElement;
  restoreEl: HTMLElement | null;
  onEscape: () => void;
}

interface FocusNavContextValue {
  focusedEl: HTMLElement | null;
  setFocus: (el: HTMLElement | null) => void;
  isActive: boolean;
  /** Restricts focus/navigation to `root`'s subtree until popped, and moves
   * focus onto the first element inside it. Escape/back then calls
   * `onEscape` instead of FocusNav's normal back handling -- see
   * popFocusScope for the other half. */
  pushFocusScope: (root: HTMLElement, onEscape: () => void) => void;
  /** Ends the most recently pushed scope and restores the focus it
   * remembered. Purely mechanical (no callback invoked) -- call this from
   * the scope owner's own cleanup effect so it runs exactly once regardless
   * of *why* the scope closed (Escape, a backdrop click, a Close button). */
  popFocusScope: () => void;
}

const FocusNavContext = createContext<FocusNavContextValue | null>(null);

export function useFocusNav(): FocusNavContextValue {
  const ctx = useContext(FocusNavContext);
  if (!ctx) throw new Error("useFocusNav must be used within FocusNavProvider");
  return ctx;
}

export function FocusNavProvider({
  resetKey,
  onBack,
  onMenu,
  children,
}: {
  /** Focus clears whenever this changes (pass the active ViewId) --
   * mirrors the reference's clear-on-route-change effect. */
  resetKey: unknown;
  /** "back" action: close a modal if one is open, else return to Library. */
  onBack: () => void;
  /** "menu" action (was TRIANGLE): return to the Home dashboard -- the same
   * destination as clicking the brand mark in TopBar, now that there's no
   * sidebar to jump focus to. */
  onMenu: () => void;
  children: ReactNode;
}) {
  const focusedRef = useRef<HTMLElement | null>(null);
  const [focusedEl, setFocusedEl] = useState<HTMLElement | null>(null);
  const isActiveRef = useRef(false);
  // Pointer visibility only (theme.css hides the real cursor under
  // [data-gamepad-active="true"]) -- tracks which input modality is
  // currently driving, independent of whether a focus ring exists. Gamepad
  // input (even a press that moves no focus, or one swallowed by a scope)
  // sets this true; real mouse movement is the only thing that clears it,
  // from every state including inside an open overlay/modal. Split out from
  // setGamepadActive below so the two can diverge: a mouse click that opens
  // an overlay still calls setFocus (isActiveRef true, a ps-focused ring is
  // legitimate) without hiding the cursor.
  const setPointerHidden = useCallback((hidden: boolean) => {
    document.documentElement.dataset.gamepadActive = hidden ? "true" : "false";
  }, []);
  // Mirrors isActiveRef onto pointer visibility -- gamepad-or-keyboard focus
  // and a visible mouse arrow are mutually exclusive states, same as
  // isActiveRef already models. Call at every isActiveRef.current assignment
  // instead of writing the ref directly. See setPointerHidden above for the
  // other, independent way the cursor is hidden (any gamepad input, focus
  // move or not).
  const setGamepadActive = useCallback((active: boolean) => {
    isActiveRef.current = active;
    setPointerHidden(active);
  }, [setPointerHidden]);
  // A single in-flight boundary-scroll retry (see processDirection below) --
  // without this, holding a direction at a scroll boundary under the
  // stick's 90ms full-tilt repeat queues a pile of overlapping polls, each
  // re-scoring the same stale boundary. Holds an rAF handle, not a
  // setTimeout id: the retry polls scrollController's isAnimating() rather
  // than guessing a fixed delay (the previous 350ms timeout could starve
  // under repeat, see this file's git history).
  const scrollRetryFrameRef = useRef<number | null>(null);
  // Up/down traversal through a ragged grid (rows whose items don't share
  // exact x-centers) remembers the column a deliberate left/right move (or
  // the first vertical move from a fresh focus) last left off, so it
  // doesn't drift sideways as it descends -- reset to null by any left/
  // right move or a view change, recaptured on the next vertical move.
  const columnAnchorXRef = useRef<number | null>(null);
  const scopeStackRef = useRef<FocusScopeEntry[]>([]);
  // View-switch focus persistence: remembers, per resetKey (ViewId), the
  // data-focus-key of whatever was focused when that view was last left, so
  // re-entering it (e.g. Home after a trip to Settings) can restore it
  // instead of always starting cold (05-acceptance-checklist's "returning
  // ... restores previous focus" applies to views, not only overlays).
  // Self-limiting: a mouse-only session never populates this (mouse
  // movement clears focusedRef without ever calling setFocus), so nothing
  // is ever restored for it either.
  const focusMemoRef = useRef<Map<unknown, string>>(new Map());
  const prevResetKeyRef = useRef<unknown>(resetKey);

  // Scoped when an overlay/modal is open: focus and navigation are confined
  // to the top scope's subtree instead of the whole document.
  const getFocusables = useCallback((): HTMLElement[] => {
    const scope = scopeStackRef.current[scopeStackRef.current.length - 1];
    const root: ParentNode = scope ? scope.root : document;
    return Array.from(root.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR)).filter((el) => {
      const rect = el.getBoundingClientRect();
      if (rect.width === 0 || rect.height === 0) return false;
      const style = window.getComputedStyle(el);
      if (style.display === "none" || style.visibility === "hidden") return false;
      if (parseFloat(style.opacity) < 0.05) return false;
      return true;
    });
  }, []);

  const findNearest = useCallback(
    (current: HTMLElement | null, direction: Direction): HTMLElement | null => {
      const all = getFocusables();
      if (!all.length) return null;

      if (!current) {
        return [...all].sort((a, b) => {
          const ar = a.getBoundingClientRect();
          const br = b.getBoundingClientRect();
          const aS = ar.top + Math.abs(ar.left + ar.width / 2 - window.innerWidth / 2) * 0.3;
          const bS = br.top + Math.abs(br.left + br.width / 2 - window.innerWidth / 2) * 0.3;
          return aS - bS;
        })[0];
      }

      const cr = current.getBoundingClientRect();
      const MIN_MOVE = 3;
      const CROSS_WEIGHT = 3;
      // A candidate entirely outside the viewport still competes (it may
      // be one scroll away and should stay reachable), but loses to any
      // on-screen candidate at a comparable distance -- the previous
      // center-distance scoring let an off-screen element win outright.
      const OFFSCREEN_PENALTY = 4000;
      const vertical = direction === "up" || direction === "down";

      if (!vertical) columnAnchorXRef.current = null;
      const anchorX = vertical ? (columnAnchorXRef.current ?? cr.left + cr.width / 2) : null;

      let best: HTMLElement | null = null;
      let bestScore = Infinity;

      for (const el of all) {
        if (el === current) continue;
        if (el.contains(current) || current.contains(el)) continue;

        const er = el.getBoundingClientRect();
        let isCandidate = false;
        let primaryDist = 0;
        let crossDist = 0;

        // Edge-to-edge (not center-to-center): a candidate qualifies only
        // once it is genuinely past current's own edge on the primary
        // axis, which is what a real grid/list traversal means -- center
        // comparison let a taller neighbor's center register as "beyond"
        // while the two rects still visibly overlapped.
        switch (direction) {
          case "up":
            isCandidate = er.bottom <= cr.top + MIN_MOVE;
            primaryDist = Math.max(0, cr.top - er.bottom);
            break;
          case "down":
            isCandidate = er.top >= cr.bottom - MIN_MOVE;
            primaryDist = Math.max(0, er.top - cr.bottom);
            break;
          case "left":
            isCandidate = er.right <= cr.left + MIN_MOVE;
            primaryDist = Math.max(0, cr.left - er.right);
            break;
          case "right":
            isCandidate = er.left >= cr.right - MIN_MOVE;
            primaryDist = Math.max(0, er.left - cr.right);
            break;
        }
        if (!isCandidate) continue;

        if (vertical) {
          // Cross axis (horizontal): gap to the column anchor, not to
          // current's own position -- 0 whenever the candidate's column
          // spans the anchor at all (projection overlap), so a same-column
          // candidate always beats an off-column one regardless of exact
          // pixel alignment.
          const ax = anchorX as number;
          crossDist = ax >= er.left && ax <= er.right ? 0 : ax < er.left ? er.left - ax : ax - er.right;
        } else {
          // Cross axis (vertical): real projection overlap between the two
          // rects, 0 whenever they share any row extent.
          const overlap = Math.min(cr.bottom, er.bottom) - Math.max(cr.top, er.top);
          crossDist = overlap > 0 ? 0 : er.top > cr.bottom ? er.top - cr.bottom : cr.top - er.bottom;
        }

        let score = primaryDist + crossDist * CROSS_WEIGHT;
        if (er.bottom < 0 || er.top > window.innerHeight || er.right < 0 || er.left > window.innerWidth) {
          score += OFFSCREEN_PENALTY;
        }
        if (score < bestScore) {
          bestScore = score;
          best = el;
        }
      }

      if (best && vertical && columnAnchorXRef.current === null) {
        columnAnchorXRef.current = anchorX;
      }
      return best;
    },
    [getFocusables],
  );

  const setFocus = useCallback((el: HTMLElement | null) => {
    if (focusedRef.current) focusedRef.current.classList.remove("ps-focused");
    focusedRef.current = el;
    setFocusedEl(el);
    if (el) {
      el.classList.add("ps-focused");
      setGamepadActive(true);
      // Owned, retargetable scroll (scrollController.ts) instead of the
      // browser's native smooth scrollIntoView -- see that module's doc for
      // why: a native smooth scroll can't be retargeted, so firing a new
      // one on every focus change under repeat made consecutive scrolls
      // fight instead of converge.
      const region = el.closest<HTMLElement>("[data-scroll-region]");
      if (region) scrollIntoViewWithin(region, el);
      playSfx("nav");
    }
  }, [setGamepadActive]);

  const pushFocusScope = useCallback(
    (root: HTMLElement, onEscape: () => void) => {
      scopeStackRef.current.push({ root, restoreEl: focusedRef.current, onEscape });
      const first = findNearest(null, "down");
      if (first) setFocus(first);
    },
    [findNearest, setFocus],
  );

  const popFocusScope = useCallback(() => {
    const top = scopeStackRef.current.pop();
    if (!top) return;
    if (top.restoreEl && document.contains(top.restoreEl)) {
      setFocus(top.restoreEl);
    } else {
      if (focusedRef.current) focusedRef.current.classList.remove("ps-focused");
      focusedRef.current = null;
      setFocusedEl(null);
    }
  }, [setFocus]);

  const processDirection = useCallback(
    (direction: Direction) => {
      if (focusedRef.current && !document.contains(focusedRef.current)) {
        focusedRef.current = null;
        setFocusedEl(null);
      }

      const next = findNearest(focusedRef.current, direction);
      if (next) {
        setFocus(next);
        return;
      }
      if (!focusedRef.current) {
        const first = findNearest(null, direction);
        if (first) setFocus(first);
        return;
      }
      // At a boundary. findScrollHost resolves the region even when it is
      // a sibling of wherever focus actually sits (GameDetail's rail vs.
      // its detail pane) -- see that function's doc; this is the fix for
      // "can't scroll the trophies list", which previously fell all the
      // way through to a no-op document.scrollingElement.
      const scope = scopeStackRef.current[scopeStackRef.current.length - 1];
      const scopeRoot: ParentNode = scope ? scope.root : document;
      const scrollHost = findScrollHost(focusedRef.current, scopeRoot);
      if (!scrollHost) return;

      const vertical = direction === "down" || direction === "up";
      const page = Math.max(160, (vertical ? scrollHost.clientHeight : scrollHost.clientWidth) * 0.6);
      const deltaTop = direction === "down" ? page : direction === "up" ? -page : 0;
      const deltaLeft = direction === "right" ? page : direction === "left" ? -page : 0;
      scrollRegionBy(scrollHost, deltaTop, deltaLeft);

      // Retry once the scroll has actually settled, polled via rAF rather
      // than guessed with a fixed delay -- the previous 350ms timeout was
      // shorter than several repeat intervals combined, so holding a
      // direction at a boundary cancelled and re-armed it repeatedly and it
      // could starve forever. isAnimating() is scrollController's own
      // source of truth for "still easing", so this always fires exactly
      // once the scroll it just started (or whatever superseded it) is
      // done, capped so a region that somehow never settles cannot hang
      // navigation.
      if (scrollRetryFrameRef.current !== null) cancelAnimationFrame(scrollRetryFrameRef.current);
      const deadline = performance.now() + 500;
      const poll = () => {
        if (isRegionAnimating(scrollHost) && performance.now() < deadline) {
          scrollRetryFrameRef.current = requestAnimationFrame(poll);
          return;
        }
        scrollRetryFrameRef.current = null;
        const retry = findNearest(focusedRef.current, direction);
        if (retry) setFocus(retry);
      };
      scrollRetryFrameRef.current = requestAnimationFrame(poll);
    },
    [findNearest, setFocus],
  );

  useEffect(() => {
    return () => {
      if (scrollRetryFrameRef.current !== null) cancelAnimationFrame(scrollRetryFrameRef.current);
    };
  }, []);

  // ---- gamepad action handling -------------------------------------------
  // Delivered as a direct callback (useGamepadActions.ts), not React state
  // -- see that hook's doc for why. handleGamepadIntent is recreated when
  // its deps change like any other useCallback; useGamepadActions captures
  // it through a ref internally, so that recreation never tears down the
  // underlying subscription.
  const handleGamepadIntent = useCallback((intent: NavIntent) => {
    // Any gamepad input is a modality signal, even one swallowed by the
    // text-entry guard below or one that moves no focus (confirm/back/menu)
    // -- setFocus() alone used to be the only thing that hid the cursor, so
    // pressing confirm/back/menu without first moving focus left the real
    // mouse arrow visible over a gamepad-driven UI.
    setPointerHidden(true);
    // The keyboard path below already guards text-entry fields (Escape and
    // the arrow keys must not fight typing); the gamepad path needs the
    // same guard -- without it, D-pad/stick navigation could steal focus
    // out of an autoFocus search field (Library's search input) while
    // someone is also holding a controller.
    const tag = (document.activeElement as HTMLElement | null)?.tagName;
    if ((tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") && intent.action !== "back") return;

    const action: FocusAction = intent.action;
    switch (action) {
      case "up":
        processDirection("up");
        break;
      case "down":
        processDirection("down");
        break;
      case "left":
        processDirection("left");
        break;
      case "right":
        processDirection("right");
        break;
      case "confirm":
        if (focusedRef.current && isActiveRef.current) {
          const el = focusedRef.current;
          playSfx("confirm");
          el.click();
          // A class, not the old string-concatenated inline transform --
          // that clobbered whatever transform the element's own focused/
          // active state already applied (e.g. a rail tile's scale(1.42)),
          // popping it back to a flat scale(0.93) instead of shrinking from
          // its actual current size. Each component composes .ps-pressed
          // with its own state class (see GameTile.module.css's
          // .active.ps-pressed for the pattern).
          el.classList.add("ps-pressed");
          setTimeout(() => {
            el.classList.remove("ps-pressed");
          }, 90);
        }
        break;
      case "back": {
        // A scope (overlay/modal) closes first, per 02-focus-hover-
        // navigation.md's Modal -> Overlay -> Current page -> Home hierarchy.
        const scope = scopeStackRef.current[scopeStackRef.current.length - 1];
        playSfx("back");
        if (scope) {
          scope.onEscape();
        } else {
          onBack();
        }
        break;
      }
      case "menu":
        onMenu();
        break;
      case "pageUp": {
        const up = findNearest(focusedRef.current, "up");
        if (up) setFocus(up);
        break;
      }
      case "pageDown": {
        const down = findNearest(focusedRef.current, "down");
        if (down) setFocus(down);
        break;
      }
    }
  }, [processDirection, onBack, onMenu, findNearest, setFocus, setPointerHidden]);
  useGamepadActions(handleGamepadIntent);

  // ---- right-stick free scroll --------------------------------------------
  // gamepad.rs emits "nav-scroll" continuously (its own 4ms active tick)
  // while the right stick is tilted, x/y already deadzone-rescaled to
  // [-1, 1] with +y = down. This was previously dead plumbing -- the Rust
  // side emitted it, inputBus.ts carried it, scrollController.ts's scrollBy
  // was even documented as being for this, but nothing ever subscribed.
  // Scaled by measured elapsed time (not a fixed per-tick constant) so the
  // scroll speed doesn't depend on how often the OS actually delivers axis
  // samples.
  useEffect(() => {
    const SCROLL_PX_PER_SEC = 900;
    let lastTs: number | null = null;
    return subscribeNavScroll((scroll: NavScroll) => {
      const now = performance.now();
      const prevTs = lastTs;
      lastTs = now;
      if (scroll.x === 0 && scroll.y === 0) return;
      // Same modality signal as handleGamepadIntent above -- a held stick
      // scrolls content under the parked pointer without ever calling
      // setFocus, so nothing else on this path would otherwise hide it.
      setPointerHidden(true);
      if (prevTs === null) return;
      const dt = Math.min(now - prevTs, 50) / 1000;
      if (!focusedRef.current) return;
      const scope = scopeStackRef.current[scopeStackRef.current.length - 1];
      const scopeRoot: ParentNode = scope ? scope.root : document;
      const host = findScrollHost(focusedRef.current, scopeRoot);
      if (!host) return;
      scrollRegionBy(host, scroll.y * SCROLL_PX_PER_SEC * dt, scroll.x * SCROLL_PX_PER_SEC * dt);
    });
  }, [setPointerHidden]);

  // ---- keyboard fallback --------------------------------------------------
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (document.activeElement as HTMLElement | null)?.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return;

      switch (e.key) {
        case "ArrowUp":
          e.preventDefault();
          processDirection("up");
          break;
        case "ArrowDown":
          e.preventDefault();
          processDirection("down");
          break;
        case "ArrowLeft":
          e.preventDefault();
          processDirection("left");
          break;
        case "ArrowRight":
          e.preventDefault();
          processDirection("right");
          break;
        case "Enter":
          if (focusedRef.current && isActiveRef.current) {
            e.preventDefault();
            const el = focusedRef.current;
            playSfx("confirm");
            el.click();
            // Same press feedback as the gamepad "confirm" case above --
            // previously only the gamepad path applied .ps-pressed, so a
            // keyboard Enter had no visible press state at all.
            el.classList.add("ps-pressed");
            setTimeout(() => {
              el.classList.remove("ps-pressed");
            }, 90);
          }
          break;
        case "Escape": {
          // A scope (overlay/modal) closes first -- see the gamepad "back"
          // case for the same hierarchy applied there.
          const scope = scopeStackRef.current[scopeStackRef.current.length - 1];
          if (scope) {
            e.preventDefault();
            scope.onEscape();
          } else if (focusedRef.current) {
            focusedRef.current.classList.remove("ps-focused");
            focusedRef.current = null;
            setFocusedEl(null);
            setGamepadActive(false);
          } else {
            playSfx("back");
            onBack();
          }
          break;
        }
        default:
          break;
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [processDirection, onBack, setGamepadActive]);

  // ---- restore cursor / deactivate on mouse movement -----------------------
  useEffect(() => {
    let debounce: ReturnType<typeof setTimeout>;
    // WebKitGTK re-fires mousemove at the pointer's last on-screen position
    // when content scrolls underneath it (gamepad d-pad/stick navigation
    // does this constantly), which is not a real mouse movement -- without
    // this coordinate check, holding a direction to scroll would flash the
    // real cursor back in on every such synthetic event.
    let lastX = -1;
    let lastY = -1;
    const onMouse = (e: MouseEvent) => {
      if (e.clientX === lastX && e.clientY === lastY) return;
      lastX = e.clientX;
      lastY = e.clientY;
      // Restore immediately and unconditionally, including while a scope
      // (overlay/modal) is open -- previously the scope guard below applied
      // to this too, so the real cursor stayed hidden inside Control
      // Center/Power Menu/any Dropdown no matter how far the mouse moved.
      setPointerHidden(false);
      // The ps-focused ring, in contrast, never clears while a scope is
      // open -- an overlay/modal keeps its own internal ring regardless of
      // stray mouse movement over it (e.g. moving the mouse to reach the
      // panel would otherwise clear the very focus that just landed there
      // on open).
      if (!isActiveRef.current || scopeStackRef.current.length > 0) return;
      clearTimeout(debounce);
      debounce = setTimeout(() => {
        if (focusedRef.current) focusedRef.current.classList.remove("ps-focused");
        focusedRef.current = null;
        setFocusedEl(null);
        setGamepadActive(false);
      }, 150);
    };
    window.addEventListener("mousemove", onMouse, { passive: true });
    return () => {
      window.removeEventListener("mousemove", onMouse);
      clearTimeout(debounce);
    };
  }, [setGamepadActive, setPointerHidden]);

  // ---- clear on view change, with per-view focus persistence --------------
  useEffect(() => {
    // A view switch (App.tsx navigates the underlying ViewId while an
    // overlay is open, e.g. Control Center's "Settings" row) outlives any
    // open scope's root -- drain the stack without running each entry's
    // onEscape (the view is already gone, there is nothing left to animate
    // closed) so a later Escape doesn't operate on a stale, unmounted root.
    scopeStackRef.current = [];
    columnAnchorXRef.current = null;

    // Remember the outgoing view's focus (by its own declared key) before
    // clearing, so returning to it later can restore it.
    const outgoingKey = focusedRef.current?.dataset.focusKey;
    if (outgoingKey) focusMemoRef.current.set(prevResetKeyRef.current, outgoingKey);

    if (focusedRef.current) focusedRef.current.classList.remove("ps-focused");
    focusedRef.current = null;
    setFocusedEl(null);
    setGamepadActive(false);

    // Restore the incoming view's remembered focus, if any and if it still
    // exists. Deferred one frame: the incoming view's own DOM may not have
    // painted under this resetKey yet on this same tick.
    const rememberedKey = focusMemoRef.current.get(resetKey);
    if (rememberedKey) {
      const raf = requestAnimationFrame(() => {
        const el = document.querySelector<HTMLElement>(`[data-focus-key="${CSS.escape(rememberedKey)}"]`);
        if (el) setFocus(el);
      });
      prevResetKeyRef.current = resetKey;
      return () => cancelAnimationFrame(raf);
    }
    prevResetKeyRef.current = resetKey;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [resetKey]);

  return (
    <FocusNavContext.Provider value={{ focusedEl, setFocus, isActive: isActiveRef.current, pushFocusScope, popFocusScope }}>{children}</FocusNavContext.Provider>
  );
}
