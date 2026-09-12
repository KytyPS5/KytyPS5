import { useLayoutEffect } from "react";

/** Drives a transform-translated rail track (03-components-and-layout.md's
 * "viewport + track" pattern), replacing `overflow-x: auto` +
 * `element.scrollIntoView` (Home.module.css's old .tileRow, FocusNav.tsx's
 * setFocus). Writes `--rail-x` on the track element so CSS does the actual
 * animating; this hook only measures and retargets.
 *
 * Centers the given slot in the viewport, clamped so the track never
 * scrolls past either end (which would reveal empty space beyond the first
 * or last tile). Measurement uses the slot's own offsetLeft/offsetWidth,
 * which transforms never affect -- so a focused tile mid-scale-transition
 * does not feed back into the offset it is itself being positioned by.
 *
 * "Latest target wins" (04-tauri-implementation.md, "avoid animation race
 * conditions"): this only ever *sets* the CSS variable, once, synchronously
 * on the relevant dependency change. A rapid A -> B -> C -> D focus
 * traversal calls this once per step, each call replacing the previous
 * target outright -- there is nothing to queue, so the one CSS transition
 * already in flight simply retargets toward wherever focus currently is.
 *
 * Takes the active slot's `data-game-index` value, not a resolved element:
 * finding that element is now this hook's own job, done inside the effect
 * below (the layout-effect phase, after commit) rather than the caller's
 * render body. Home.tsx previously ran `track.querySelector(...)` directly
 * in its component body on every render to produce the element this hook
 * wanted -- a DOM read during render, forcing a style/layout resolution
 * React's render phase is supposed to stay free of, and one this hook was
 * about to duplicate again internally regardless. */
export function useRailOffset(viewportRef: React.RefObject<HTMLElement | null>, trackRef: React.RefObject<HTMLElement | null>, activeIndex: number | null | undefined): void {
  useLayoutEffect(() => {
    const viewport = viewportRef.current;
    const track = trackRef.current;
    if (!viewport || !track) return;

    const activeSlot = activeIndex != null ? track.querySelector<HTMLElement>(`[data-game-index="${activeIndex}"]`) : null;

    const viewportWidth = viewport.clientWidth;
    const trackWidth = track.scrollWidth;

    let targetX = 0;
    if (activeSlot && track.contains(activeSlot)) {
      const slotCenter = activeSlot.offsetLeft + activeSlot.offsetWidth / 2;
      targetX = viewportWidth / 2 - slotCenter;
    }

    // Clamp: never pull the track's trailing edge inward of the viewport's
    // trailing edge, and never push its leading edge past 0 -- both ends
    // stay flush with real content instead of exposing blank track.
    const minX = Math.min(0, viewportWidth - trackWidth);
    targetX = Math.max(minX, Math.min(0, targetX));

    track.style.setProperty("--rail-x", `${targetX}px`);
    // Dependency array added: without one, this useLayoutEffect re-ran
    // after every single render of the parent (Home.tsx re-renders on
    // every hover move), forcing a synchronous clientWidth/scrollWidth
    // layout read each time regardless of whether activeSlot had actually
    // changed -- a real read-then-write thrash on the hottest path in the
    // app. activeSlot is the only reactive input this effect cares about;
    // viewportRef/trackRef are refs (stable identity across renders, not
    // meaningful dependencies).
  }, [activeIndex]);
}
