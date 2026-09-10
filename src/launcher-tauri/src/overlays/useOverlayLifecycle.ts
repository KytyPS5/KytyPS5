import { useEffect, useState } from "react";

/** Keeps an overlay mounted for `exitMs` after `open` goes false, so its CSS
 * exit transition (Overlay.module.css's .backdrop/.panel) actually gets to
 * play instead of the element vanishing the instant React re-renders --
 * "overlay exit reverses the entrance" (05-acceptance-checklist.md).
 * `isOpen` drives the CSS class; `mounted` is whether to render the overlay
 * at all. */
export function useOverlayLifecycle(open: boolean, exitMs: number): { mounted: boolean; isOpen: boolean } {
  const [mounted, setMounted] = useState(open);
  const [isOpen, setIsOpen] = useState(open);

  useEffect(() => {
    if (open) {
      setMounted(true);
      // One rAF so the initial (closed) styles paint before flipping to
      // open -- otherwise there is nothing for the transition to animate
      // from and the entrance pops instead of animating in.
      const raf = requestAnimationFrame(() => setIsOpen(true));
      return () => cancelAnimationFrame(raf);
    }
    setIsOpen(false);
    const timer = setTimeout(() => setMounted(false), exitMs);
    return () => clearTimeout(timer);
  }, [open, exitMs]);

  return { mounted, isOpen };
}
