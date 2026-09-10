import { useEffect, useState } from "react";
import { readTimeFormat, useActiveProfile } from "./profiles";

/** Ticking wall-clock string for the top bar, e.g. "14:32". Ported from the
 * one-shot `new Date().toLocaleTimeString()` call in the old Library header
 * (views/Library.tsx), that one never updated after mount; this one does.
 *
 * hour12 is now explicit from the active profile's timeFormat (default
 * 24h), not left to toLocaleTimeString's locale-dependent guess -- that
 * previously showed 12h AM/PM on any system whose locale defaults to it,
 * with no way to change it from inside the app. */
export function useClock(): string {
  const timeFormat = readTimeFormat(useActiveProfile());
  const [now, setNow] = useState(() => formatTime(new Date(), timeFormat));

  useEffect(() => {
    // Also fires immediately on a timeFormat change, not just every 15s --
    // otherwise switching the profile setting wouldn't visibly apply until
    // the next tick.
    setNow(formatTime(new Date(), timeFormat));
    const id = setInterval(() => setNow(formatTime(new Date(), timeFormat)), 15_000);
    return () => clearInterval(id);
  }, [timeFormat]);

  return now;
}

// Manual formatting, not toLocaleTimeString({ hour12 }) -- confirmed real
// bug (2026-09-09): picking 24h in Settings had no effect on the TopBar
// clock, which kept showing 12h AM/PM. The dev server was serving the
// updated module (checked directly), and Node's own Intl gives the correct
// result for the same options, so this isn't stale code or a logic error
// here -- it's this app's embedded WebKitGTK/JavaScriptCore build not
// honoring `hour12` the way the spec (and V8) does. Sidestepping Intl
// entirely removes the dependency on that engine behavior.
function formatTime(d: Date, format: "24h" | "12h"): string {
  const pad = (n: number) => String(n).padStart(2, "0");
  if (format === "24h") {
    return `${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }
  const hour24 = d.getHours();
  const hour12 = hour24 % 12 === 0 ? 12 : hour24 % 12;
  const suffix = hour24 < 12 ? "AM" : "PM";
  return `${pad(hour12)}:${pad(d.getMinutes())} ${suffix}`;
}
