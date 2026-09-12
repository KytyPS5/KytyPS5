/** Owned, retargetable scroll animation, replacing the previous reliance on
 * the browser's native `scrollIntoView({behavior:"smooth"})` /
 * `scrollBy({behavior:"smooth"})`. A native smooth scroll cannot be
 * retargeted mid-flight -- issuing a second one while the first is still
 * running (exactly what happens holding a direction at the 90ms repeat
 * rate) makes the two fight, and neither ever settles. This module keeps
 * exactly one target `{top, left}` per scrollable region and integrates
 * every active region toward its target on one shared rAF loop; retargeting
 * is just overwriting that one value ("latest target wins", the same
 * pattern lib/useRailOffset.ts already proved for the transform-driven
 * rail). The loop stops itself once every region has settled, so this costs
 * nothing at rest.
 *
 * Regions are tracked in a WeakMap keyed by the scroll element itself --
 * no register/unregister lifecycle to wire into every scrollable view; an
 * element that unmounts simply falls out of both the WeakMap and the
 * `active` set (the latter is a plain Set, so removal happens explicitly
 * once the region settles or is asked to stop, not via GC -- see
 * `forget`). */

interface RegionState {
  targetTop: number;
  targetLeft: number;
}

const regions = new WeakMap<HTMLElement, RegionState>();
const active = new Set<HTMLElement>();
let rafId = 0;

const SETTLE_EPSILON_PX = 0.5;
// Fraction of the remaining distance closed per frame -- an exponential
// ease that always converges and is always coherent to retarget, unlike a
// fixed-duration animation that has to be cancelled and restarted.
const EASE = 0.25;

function tick(): void {
  let stillActive = false;
  for (const el of active) {
    const state = regions.get(el);
    if (!state || !el.isConnected) {
      active.delete(el);
      continue;
    }
    const dTop = state.targetTop - el.scrollTop;
    const dLeft = state.targetLeft - el.scrollLeft;
    if (Math.abs(dTop) < SETTLE_EPSILON_PX && Math.abs(dLeft) < SETTLE_EPSILON_PX) {
      if (dTop !== 0) el.scrollTop = state.targetTop;
      if (dLeft !== 0) el.scrollLeft = state.targetLeft;
      active.delete(el);
      continue;
    }
    el.scrollTop += dTop * EASE;
    el.scrollLeft += dLeft * EASE;
    stillActive = true;
  }
  rafId = stillActive ? requestAnimationFrame(tick) : 0;
}

function ensureLoop(): void {
  if (!rafId) rafId = requestAnimationFrame(tick);
}

function clamp(value: number, max: number): number {
  return Math.max(0, Math.min(value, Math.max(0, max)));
}

/** Sets (or retargets) where `region` should scroll to. Safe to call on
 * every repeat tick -- this never starts a new animation that competes with
 * one already running, it only updates the single target the shared loop
 * is continuously easing toward. */
export function setScrollTarget(region: HTMLElement, top: number, left: number): void {
  const clampedTop = clamp(top, region.scrollHeight - region.clientHeight);
  const clampedLeft = clamp(left, region.scrollWidth - region.clientWidth);
  regions.set(region, { targetTop: clampedTop, targetLeft: clampedLeft });
  active.add(region);
  ensureLoop();
}

/** Nudges `region`'s target by a relative amount (right-stick free scroll,
 * or a page-jump with no specific element to align to), clamped the same
 * way as an absolute target. */
export function scrollBy(region: HTMLElement, deltaTop: number, deltaLeft: number): void {
  const current = regions.get(region);
  const fromTop = current ? current.targetTop : region.scrollTop;
  const fromLeft = current ? current.targetLeft : region.scrollLeft;
  setScrollTarget(region, fromTop + deltaTop, fromLeft + deltaLeft);
}

/** Computes, once, the delta needed to bring `el` fully into `region`'s
 * visible box (matching `scrollIntoView`'s `block: "nearest"` semantics --
 * only moves when `el` is actually clipped, and moves the minimum amount),
 * then hands the result to the owned easing loop above. A single
 * `getBoundingClientRect()` pair-read at the moment focus changes reflects
 * `el`'s true current rendered position (including any in-flight focus
 * transform) correctly -- unlike the old approach, this is never re-read
 * while a previous *scroll* is still in flight, because that previous
 * scroll is this same owned, always-coherent target instead of a second
 * independent native animation. */
export function scrollIntoViewWithin(region: HTMLElement, el: HTMLElement, margin = 12): void {
  const regionRect = region.getBoundingClientRect();
  const elRect = el.getBoundingClientRect();

  let nextTop = region.scrollTop;
  if (elRect.top < regionRect.top + margin) {
    nextTop = region.scrollTop + (elRect.top - regionRect.top) - margin;
  } else if (elRect.bottom > regionRect.bottom - margin) {
    nextTop = region.scrollTop + (elRect.bottom - regionRect.bottom) + margin;
  }

  let nextLeft = region.scrollLeft;
  if (elRect.left < regionRect.left + margin) {
    nextLeft = region.scrollLeft + (elRect.left - regionRect.left) - margin;
  } else if (elRect.right > regionRect.right - margin) {
    nextLeft = region.scrollLeft + (elRect.right - regionRect.right) + margin;
  }

  setScrollTarget(region, nextTop, nextLeft);
}

/** Whether `region` is still easing toward its target. FocusNav's boundary
 * handler polls this (via rAF, not a fixed timeout) to know when a
 * boundary-scroll has actually settled before re-scoring candidates --
 * scoring against a region still mid-scroll would read transient
 * geometry. */
export function isAnimating(region: HTMLElement): boolean {
  return active.has(region);
}

/** Stops animating and forgets `region` -- call when a region is about to
 * unmount somewhere the WeakMap's natural GC would not run soon enough to
 * matter (not required for correctness, `tick`'s `el.isConnected` check
 * already handles a region disappearing mid-animation safely). */
export function forgetScrollRegion(region: HTMLElement): void {
  active.delete(region);
  regions.delete(region);
}
