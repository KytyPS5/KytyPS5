import { useEffect, useState } from "react";
import { cssUrl } from "../lib/cssUrl";
import styles from "./HeroBackground.module.css";

/** Full-bleed art + scrims + accent radial for the dashboard, mounted once
 * by AppShell (never per-view -- see store/background.ts) so it survives
 * navigation instead of restarting from black every time.
 *
 * `baseUrl` is the selected dashboard background (Settings > Background),
 * painted at opacity 1 with no transition -- it is the floor everything
 * else sits on, and the only way this component ever shows flat --void is
 * if baseUrl itself fails to load (background.ts's useVerifiedBaseUrl
 * guards that). `heroUrl` is the current game's own art, crossfaded on top
 * of the base only once it is confirmed to load; null hides it and reveals
 * the base again.
 *
 * Two hero layers stay permanently mounted; only their opacity toggles when
 * `heroUrl` changes -- this is the exact shape the Step 0b perf spike
 * measured at 62.6fps (vs. 13.3fps for AnimatePresence mode="wait", which
 * unmounts the outgoing layer first, producing a fade-to-empty rather than
 * a crossfade).
 *
 * `baseUrl`/`heroUrl` should already be final display URLs -- pre-blurred
 * where needed (see lib/accent.ts's useHeroDisplayUrl) -- this component
 * never applies a live CSS filter itself. */
export function HeroBackground({ baseUrl, heroUrl, accent }: { baseUrl: string; heroUrl: string | null; accent: string }) {
  const [slotUrls, setSlotUrls] = useState<[string | null, string | null]>([null, null]);
  const [activeSlot, setActiveSlot] = useState<0 | 1>(0);
  const [heroVisible, setHeroVisible] = useState(false);

  // Same two-slot crossfade shape as the hero image layers below, applied
  // to the accent radial so its color change animates via opacity (see
  // HeroBackground.module.css's .accentRadial/.accentRadialActive) instead
  // of transitioning the `background` gradient string itself. No load gate
  // needed here (unlike the hero image): a gradient has no decode step, so
  // there is nothing to wait on before it is safe to paint.
  const [accentSlots, setAccentSlots] = useState<[string, string]>([accent, accent]);
  const [activeAccentSlot, setActiveAccentSlot] = useState<0 | 1>(0);
  useEffect(() => {
    if (accent === accentSlots[activeAccentSlot]) return;
    const nextSlot: 0 | 1 = activeAccentSlot === 0 ? 1 : 0;
    setAccentSlots((prev) => {
      const copy: [string, string] = [...prev];
      copy[nextSlot] = accent;
      return copy;
    });
    const raf = requestAnimationFrame(() => setActiveAccentSlot(nextSlot));
    return () => cancelAnimationFrame(raf);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [accent]);

  useEffect(() => {
    if (!heroUrl) {
      setHeroVisible(false);
      return;
    }
    if (heroUrl === slotUrls[activeSlot]) {
      setHeroVisible(true);
      return;
    }
    const nextSlot: 0 | 1 = activeSlot === 0 ? 1 : 0;
    let cancelled = false;
    // Only promote a slot to active once the browser has actually decoded
    // the image -- painting it as a plain CSS background-image fires no
    // load/error event of its own, so a bare rAF here (the previous
    // approach) would flip to "active" regardless of whether anything
    // rendered, showing the base layer's absence (i.e. --void, before this
    // component had a base layer at all) for the length of a failing or
    // slow request. On error, fall back to the base layer (setHeroVisible
    // false) rather than leaving the outgoing game's art in place -- real
    // bug (2026-09-10): a dead/unreadable art path left the PREVIOUS game's
    // hero stuck on screen forever, since nothing ever cleared it.
    const img = new Image();
    img.onload = () => {
      if (cancelled) return;
      setSlotUrls((prev) => {
        const copy: [string | null, string | null] = [...prev];
        copy[nextSlot] = heroUrl;
        return copy;
      });
      // Let the browser paint the new image in the inactive slot at
      // opacity 0 before flipping it to active -- otherwise the transition
      // has nothing to transition from and the swap pops instead of
      // crossfading.
      requestAnimationFrame(() => {
        if (cancelled) return;
        setActiveSlot(nextSlot);
        setHeroVisible(true);
      });
    };
    img.onerror = () => {
      if (cancelled) return;
      setHeroVisible(false);
    };
    img.src = heroUrl;
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [heroUrl]);

  return (
    <div className={styles.root}>
      <div className={styles.base} style={{ backgroundImage: cssUrl(baseUrl) }} />
      {([0, 1] as const).map((slot) => {
        const slotUrl = slotUrls[slot];
        if (!slotUrl) return null;
        const active = heroVisible && slot === activeSlot;
        return <div key={slot} className={active ? styles.layerActive : styles.layer} style={{ backgroundImage: cssUrl(slotUrl) }} />;
      })}
      <div className={styles.scrimRight} />
      <div className={styles.scrimTop} />
      {/* Corner-darkening vignette, the Background-component responsibility
         03-components-and-layout.md lists alongside crossfade/scrim/blur --
         a static radial gradient, nothing to profile. */}
      <div className={styles.vignette} />
      {([0, 1] as const).map((slot) => (
        <div
          key={slot}
          className={slot === activeAccentSlot ? styles.accentRadialActive : styles.accentRadial}
          style={{ background: `radial-gradient(ellipse at 20% 80%, ${accentSlots[slot]}22 0%, transparent 60%)` }}
        />
      ))}
    </div>
  );
}
