import { useEffect, useState } from "react";
import type { HeroArt } from "./heroArt";

// Per-game accent color extraction, and a blur-cache for hero art that
// needs it (see heroArt.ts's needsBlur). Both need a decoded bitmap of the
// same source image, so they share one loader and one cache entry per URL.
//
// Tainting: convertFileSrc() yields `asset://localhost/...` on Linux, a
// different origin from both the dev server and the packaged app, so
// `<img crossOrigin>` + canvas is the flaky path (WebKitGTK's custom-scheme
// CORS handling). fetch() -> blob() -> createImageBitmap() instead: a
// blob: URL is same-origin, so the canvas this produces is never tainted,
// and a fetch failure is a catchable rejection rather than a silent throw
// deep inside getImageData().

const FALLBACK_ACCENT = "#0070d1"; // var(--accent)
const BLUR_PX = 18;
const BLUR_SCALE = 1.1; // matches the CSS transform scale that hides the blur's soft edge
// The blur render below draws at this size (longest edge, before
// BLUR_SCALE) instead of the source bitmap's own resolution (icon0.png-
// derived hero art runs up to 1024x1024). An 18px blur destroys detail at
// that frequency regardless of source size, so a small intermediate is
// visually identical once CSS scales the result back up to fill the
// viewport -- and cuts the blur+encode's pixel count by roughly 20x,
// which is what actually mattered: this ran synchronously on the main
// thread per title on first hover, and was the confirmed source of the
// "hovering between objects feels stuck" hitch.
const BLUR_SOURCE_MAX_PX = 220;

interface CacheEntry {
  accent: string;
  blurredUrl?: string;
}

const cache = new Map<string, Promise<CacheEntry>>();
const HUE_STEP = 47; // coprime-ish spread over 360 for a hash-based fallback hue

function hashHue(key: string): number {
  let h = 0;
  for (let i = 0; i < key.length; i++) h = (h * 31 + key.charCodeAt(i)) | 0;
  return ((Math.abs(h) * HUE_STEP) % 360);
}

async function loadBitmap(url: string): Promise<ImageBitmap> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch ${url} failed: ${res.status}`);
  const blob = await res.blob();
  return createImageBitmap(blob);
}

function extractAccent(bitmap: ImageBitmap): string {
  const size = 16;
  const canvas = new OffscreenCanvas(size, size);
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("2d context unavailable");
  ctx.drawImage(bitmap, 0, 0, size, size);
  const { data } = ctx.getImageData(0, 0, size, size);

  let r = 0;
  let g = 0;
  let b = 0;
  let n = 0;
  for (let i = 0; i < data.length; i += 4) {
    const [pr, pg, pb] = [data[i], data[i + 1], data[i + 2]];
    const max = Math.max(pr, pg, pb);
    const min = Math.min(pr, pg, pb);
    const lightness = (max + min) / 2 / 255;
    const saturation = max === min ? 0 : (max - min) / 255;
    // Skip near-black, near-white, and low-saturation pixels -- without
    // this a typical dark game-icon border averages to grey for nearly
    // every game (measured against the real icon0.png assets on disk).
    if (lightness < 0.12 || lightness > 0.92 || saturation < 0.15) continue;
    r += pr;
    g += pg;
    b += pb;
    n++;
  }
  if (n === 0) return FALLBACK_ACCENT;
  r = Math.round(r / n);
  g = Math.round(g / n);
  b = Math.round(b / n);
  return `#${[r, g, b].map((v) => v.toString(16).padStart(2, "0")).join("")}`;
}

async function renderBlurred(bitmap: ImageBitmap): Promise<string> {
  // Downscale FIRST, then blur -- see BLUR_SOURCE_MAX_PX's doc. The blur
  // radius is scaled down by the same factor so the visual softness ratio
  // (relative to content) stays the same after CSS scales the small
  // result back up, instead of reading as a proportionally heavier blur.
  const downscale = Math.min(1, BLUR_SOURCE_MAX_PX / Math.max(bitmap.width, bitmap.height));
  const w = Math.round(bitmap.width * downscale * BLUR_SCALE);
  const h = Math.round(bitmap.height * downscale * BLUR_SCALE);
  const canvas = new OffscreenCanvas(w, h);
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("2d context unavailable");
  // One-time blur render, not a live CSS filter -- see the Step 0b
  // measurement (13fps vs 62fps) for why this must never run per-frame.
  ctx.filter = `blur(${BLUR_PX * downscale}px)`;
  ctx.drawImage(bitmap, 0, 0, w, h);
  const blob = await canvas.convertToBlob({ type: "image/png" });
  return URL.createObjectURL(blob);
}

async function process(url: string): Promise<CacheEntry> {
  try {
    const bitmap = await loadBitmap(url);
    try {
      const [accent, blurredUrl] = await Promise.all([
        Promise.resolve().then(() => extractAccent(bitmap)),
        renderBlurred(bitmap),
      ]);
      return { accent, blurredUrl };
    } finally {
      bitmap.close();
    }
  } catch {
    return { accent: hashHueToHex(url) };
  }
}

function hashHueToHex(key: string): string {
  const hue = hashHue(key);
  // Fixed, generous saturation/lightness so the fallback always reads as a
  // usable UI accent rather than something washed-out or neon.
  return hslToHex(hue, 55, 50);
}

function hslToHex(h: number, s: number, l: number): string {
  s /= 100;
  l /= 100;
  const k = (n: number) => (n + h / 30) % 12;
  const a = s * Math.min(l, 1 - l);
  const f = (n: number) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)));
  const toHex = (n: number) => Math.round(f(n) * 255).toString(16).padStart(2, "0");
  return `#${toHex(0)}${toHex(8)}${toHex(4)}`;
}

/** Accent color for a hero art URL, cached per URL for the session. Falls
 * back to a hash-derived hue (never a flat default) if extraction fails --
 * see `process`'s catch. */
export async function getAccentColor(url: string): Promise<string> {
  let entry = cache.get(url);
  if (!entry) {
    entry = process(url);
    cache.set(url, entry);
  }
  return (await entry).accent;
}

/** Pre-blurred data/blob URL for square-sourced hero art (see
 * HeroArt.needsBlur). Undefined if extraction failed -- callers should fall
 * back to the raw url with no blur rather than block on it. */
export async function getBlurredUrl(url: string): Promise<string | undefined> {
  let entry = cache.get(url);
  if (!entry) {
    entry = process(url);
    cache.set(url, entry);
  }
  return (await entry).blurredUrl;
}

/** Accent color for the current hero art, re-derived when the art changes. */
export function useAccentColor(art: HeroArt | null): string {
  const [accent, setAccent] = useState(FALLBACK_ACCENT);

  useEffect(() => {
    if (!art) {
      setAccent(FALLBACK_ACCENT);
      return;
    }
    let cancelled = false;
    void getAccentColor(art.url).then((c) => {
      if (!cancelled) setAccent(c);
    });
    return () => {
      cancelled = true;
    };
  }, [art?.url]);

  return accent;
}

/** The URL HeroBackground should actually paint: the pre-blurred derivative
 * when the source needs one, the raw url once blurring fails or isn't
 * needed. Stays null while a needsBlur source is still awaiting its blur
 * render rather than showing the sharp square first -- the shell's always-
 * on base layer (store/background.ts, HeroBackground.tsx's .base) covers
 * that short gap, so there is nothing left to hide by showing the
 * unblurred source early, and skipping it means one crossfade per title
 * instead of a sharp-then-blurred double-pump. */
export function useHeroDisplayUrl(art: HeroArt | null): string | null {
  const [url, setUrl] = useState<string | null>(null);

  useEffect(() => {
    if (!art) {
      setUrl(null);
      return;
    }
    if (!art.needsBlur) {
      setUrl(art.url);
      return;
    }
    setUrl(null);
    let cancelled = false;
    void getBlurredUrl(art.url).then((blurred) => {
      if (!cancelled) setUrl(blurred ?? art.url);
    });
    return () => {
      cancelled = true;
    };
  }, [art?.url, art?.needsBlur]);

  return url;
}
