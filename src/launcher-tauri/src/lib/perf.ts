/** Boot-time frame-time probe. Runs a short rAF sample on startup and sets
 * `data-perf="low"` on the document root when the median frame time is
 * rough -- CSS (theme.css, GlassPanel.module.css) reacts to that attribute
 * by dropping backdrop-filter to a flat fill. This is the degradation
 * switch for hardware below the dev machine the Step 0b measurement (62.6fps
 * mitigated / 13.3fps naive) was taken on; it does not change which stack is
 * the default -- the mitigated stack always runs, this just adds a further
 * fallback under it. */

const SAMPLE_FRAMES = 40;
const LOW_PERF_MEDIAN_MS = 20; // ~50fps or worse

export function runPerfProbe(): void {
  const samples: number[] = [];
  let last = 0;

  const tick = (t: number) => {
    if (last) samples.push(t - last);
    last = t;
    if (samples.length < SAMPLE_FRAMES) {
      requestAnimationFrame(tick);
      return;
    }
    samples.sort((a, b) => a - b);
    const median = samples[Math.floor(samples.length / 2)];
    if (median > LOW_PERF_MEDIAN_MS) {
      document.documentElement.dataset.perf = "low";
    }
  };
  requestAnimationFrame(tick);
}
