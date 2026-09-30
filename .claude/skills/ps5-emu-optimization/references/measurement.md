# Measurement and iteration methodology

## Harness to build (once)

1. **Fixed save swap**: a script that moves the player's save aside, installs a baseline save,
   reinstalls it fresh before each run, and restores the player's save with a per-file SHA-256 check.
   It refuses to restore while an emulator is running.
2. **Scripted startup**: launch, skip logos/cinematics, detect "in game" from pixel statistics of a
   client-area capture (e.g. the HUD), with a startup timeout (startup > 1 min is a bug, not noise).
3. **Scripted input from one process** (a whole key plan like `w:down:0,h:down:4000,h:up:4500,w:up:25000`).
   Separate key processes drifted up to 1 s and the route ended at different walls.
4. **Live control channel**: the emulator polls a command file (~100 ms) and answers with tagged log
   lines. Commands used: `measure <s> <label>` (fps, render ms, counters), `poke32 <addr> <val>`
   (flip a switch in the running process), wall-clock thread sampling, event timeline trace, per-kind
   render-thread census, time-HLE census (which guest call sites sleep/wait), PGO profile dump.
5. **Symbolization**: link with a map file and a fixed image base; keep a copy of the exe + map per
   build; profiles and crash addresses are only valid against the build that ran.
6. **Reports**: per-second fps table, worst 1-s window and 1% low, render-idle vs GPU-busy split,
   callee split of one function, slow-call timeline (log lines for calls over N ms, naming the resource).

## A/B protocol

- **Same process, alternating**: toggle the switch live (A B A B ..., several rounds of 6-20 s) at a
  fixed spot. Idle noise ~+-0.1 fps. Cross-session comparisons vary +-1-3 fps with where the character
  stops; moving routes vary +-1-2 fps run to run, so use several runs and per-second tables.
- Aggregate as total frames / total time. Keep every window; never cherry-pick outliers.
- Record CPU temperature (a hot package throttled ~7%). Don't benchmark while compiling.
- An old binary is A/B'd from its own directory under the same exe name (tools find it by name).
- Measure at least two scenes (a standing spot and a moving route) and check screenshots each time.

## What counts as proof

- Causal slope first (inject delay, measure frame-time response) for any "saves X ms" claim.
- Upper bound first (skip the work unsafely), then the real replacement.
- Correctness gates, all required for a keep:
  - verify mode for replaced work: 0 mismatches over a long run (e.g. 420K compares);
  - offline replay tests of captured draws, pixel-identical;
  - screenshots within animation noise at the start and during the run;
  - a 10-minute scripted playtest without crash;
  - the unit/regression suite with no new failures (keep a list of known failures).
- Instrumentation never counts: scoped timers added +32%; diagnostics must be free when off.

## Iteration loop

1. Profile -> pick the largest category on the critical thread.
2. Causal probe at that site.
3. Upper bound experiment.
4. Implement behind a switch (+ verify mode).
5. Same-process ABBA at two scenes + screenshots.
6. Keep (make unconditional, remove the switch) or delete the code. Log the numbers either way.
7. Re-profile: the bottleneck moves (translation-bound windows become GPU-bound; savings on async
   queues vanish).

## Stutter-specific measurement

- Cold runs need a new binary identity and empty caches; the GPU driver's own shader disk cache warms
  repeated "cold" runs too. Compare which modules were compiled at run time against the precompiled
  set instead of counting stalls.
- Log slow calls with the resource name and put them on the walk timeline; big-image churn
  (untrack/full-dirty/unmap-delete lines) explains most streaming hitches.

## Safety rules for automated runs

- Only one emulator instance at a time; scripts kill only the process they started (a late
  kill-by-name from an earlier job once killed a fresh run).
- A crashed process can linger holding the exe; rename the exe to rebuild.
- Don't fight the user for window focus; flag "focus lost: route not trusted" instead.
