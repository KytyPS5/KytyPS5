import { useEffect, useState } from "react";
import { createStore, useStore } from "./observable";
import { THEMES, useDashboardTheme } from "../lib/theme";

/** Shell-level background state. The selected dashboard theme is read
 * straight off lib/theme.ts (Settings owns that); this store only carries
 * the one thing views need to layer on top of it -- the active game's own
 * hero art, published by Home while it is mounted, cleared the moment Home
 * unmounts so every other view falls back to the theme (real bug,
 * 2026-09-09: per-view HeroBackground instances meant this state died on
 * every navigation, restarting the fade-from-black each time). */
export interface HeroOverride {
  url: string;
  accent: string;
}

const heroOverrideStore = createStore<HeroOverride | null>(null);

export function setHeroOverride(v: HeroOverride | null): void {
  heroOverrideStore.set(v);
}

export function clearHeroOverride(): void {
  heroOverrideStore.set(null);
}

/** Verifies theme.url actually loads before trusting it as the base layer --
 * a stale custom-background path (file moved/deleted after being picked)
 * otherwise falls straight through to flat --void with no recovery, since
 * lib/theme.ts's convertFileSrc() has no way to know the file is gone. Falls
 * back to THEMES[0] (Nebula), the same default readStored() uses. */
function useVerifiedBaseUrl(url: string): string {
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    setFailed(false);
    let cancelled = false;
    const probe = new Image();
    probe.onerror = () => {
      if (!cancelled) setFailed(true);
    };
    probe.src = url;
    return () => {
      cancelled = true;
    };
  }, [url]);

  return failed && url !== THEMES[0].url ? THEMES[0].url : url;
}

/** The one background HeroBackground actually needs: baseUrl is the
 * verified, always-shown selected background; heroUrl is the current game's
 * own art, or null to show only the base. */
export function useBackground(): { baseUrl: string; heroUrl: string | null; accent: string } {
  const theme = useDashboardTheme();
  const override = useStore(heroOverrideStore);
  const baseUrl = useVerifiedBaseUrl(theme.url);

  return {
    baseUrl,
    heroUrl: override?.url ?? null,
    accent: override?.accent ?? theme.accent,
  };
}
