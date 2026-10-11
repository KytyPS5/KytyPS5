# Ghost of Yōtei: active integration and session resume

Checkpoint: 2026-10-11. Primary execution path is now the isolated branch
yotei-integration-native based on peepostarexx/YOTEI-BRANCH6d041035.
The previous branch remains intact as a source of verified tests and mechanisms.

## Open these paths first

- Primary worktree: G:\repos\KytyPS5\_Build\worktrees\yotei-fork-baseline-6d041035
- Primary branch: yotei-integration-native
- Tested engine revision: 6d04103525b24559a496612ef8959a7666ce9a20
- Previous checkout: G:\repos\KytyPS5, branch yotei-windows-bringup
- Previous completed workflow commit: 85494f562070a2d245e79dea4b43931d6d5d6baa
- Main progress/history: G:\repos\KytyPS5\docs\ghost-of-yotei-windows-plan.md
- Base research/decision: G:\repos\KytyPS5\docs\yotei-base-comparison-plan-2026-10-11.md
- Shared context: C:\Users\fxpw\.codex\shared-context\README.md and newest KytyPS5 handoff
- WSL access: /mnt/g/repos/KytyPS5 and /mnt/c/Users/fxpw/.codex/shared-context

A session starting in the previous checkout must explicitly use the primary
worktree for new integration work. Do not replace the previous branch or merge
the divergent histories wholesale. Documentation commits on the primary branch
do not change the engine code that was tested at6d041035.

## Verified state

Fork native build and run:
- Private executable: G:\repos\KytyPS5\_Build\analysis\yotei-fork-verification-20261011\runtime\kyty_emulator.exe
- SHA256: b3ed953555e91759b798aced8a04af324508ceaafd25a70a34d6d34c879c96ef
- Run: _Build/runs/game-20261011-133628-fork-6d041035-baseline-fb3974 in the previous checkout
- Visually confirmed interactive introduction with writing-names prompt and
  SETTINGS/CONTROLS menu after Enter/Options. Character movement, open world and
  title screen remain pending. Observed6.2/9.1FPS is not a measured benchmark.
- Original game01.512, RTX5060Ti/Ryzen5950X, Performance1280x720,
  bindless + linear-image readback + raw image/buffer synchronization.
- Finished/drained14:06:33UTC:1800s timeout, graceful exit0, no memory guard,
  no forced cleanup/reset, minimum free RAM9.86GiB.
- Driver cache payload265360493bytes; disk265360640bytes.
- Full evidence: _Build/analysis/yotei-fork-verification-20261011/final-baseline-20261011T141306Z.md

Previous branch diagnostic workflow:
- Native capture6/6; unchanged fused front/back audit RED -> GREEN2/2;
  missing/corrupt inputs6/6 correctly classified as input_error.
- PowerShell5.1 orphan cleanup intended RED -> unchanged GREEN.
- Corpus825 distinct partial profiles:777 passes,47 failed+1timeout in39.048s.
  These are CFG/resource planning checks, not825 successful shader generations.
- Fifteen reason groups overlap68times. Reconciled48 inputs:
  RT19, HS-front input-boundary artifact6, resources9, dynamic PC7,
  FP64 literal4, FLAT D16 1, SharedBase1, timeout1.
- Reports and test-only FP64 draft: _Build/analysis/shader-batch-workflow-20261011.
  The FP64 literal draft is unapplied/unvalidated. The new fork already has a
  literal implementation; reassess groups on the new base before choosing fixes.

Previous native capture run:
- _Build/runs/game-20261011-142826-shader-batch-capture-warm
- Engine85494f56, private EXE975bdff7786276b8c96e182c858504e27c30d1f52bd3a0f404660f425327cdf5
- Finished/drained14:59:27UTC, graceful0 after1800s timeout; minfree8.88GiB,
  no memory guard/forced cleanup/reset. Owned screenshot remains black.
- Large modules346832/521303words; driver cache payload1236842005bytes.
  These measurements suggest expensive generation/compilation; causality and
  equal-interval performance comparison still need proof.
-5215 captures:4096 compiler requests +1119 runtime variants. Cached user-data
  changes exhausted the compiler quota before late real generation143514.
  Source capture-admission follow-up is UNVALIDATED WIP in the previous branch:
  archive actual Translate/Emit attempts, defer successful cached transcripts,
  protect failure capacity and preserve exact policy/push/back metadata.
  Inspect the newest handoff/status/diff before applying, building or committing it.

## Next actions

1. Save/review the pending capture-admission delta and its exact validation state.
   Run its meaningful CPU coordinator/reserve tests on the previous branch before
   treating it as a completed fix. Transfer compatible diagnostic pieces as small
   reviewed ports to the primary branch; keep original engine behavior unchanged.
2. Retry the primary native artifact with warm cache and bounded lifetime; verify
   mouse/touchpad input in the interactive introduction, then actual character
   movement and entry into a scene. Record screenshots, input, exact revision and
   executable hash. Bindless placeholders/default samplers remain experimental.
3. After the saved plan, implement the user-requested batch collection with
   diagnostic fallback. Capture the original failure and full input BEFORE using
   a fallback; retain stage, main/back bytes, user data, host/static policy,
   push offset and bounded runtime read transcript. Batch independent inputs,
   group shared reasons and preserve overlapping counts and replay commands.
4. Prefer existing valid alternatives such as structured-control dispatcher
   fallback or the original validated SPIR-V after optimizer failure. Keep
   experimental resource substitutions explicit. Never submit invalid SPIR-V or
   unbounded GPU loops. If no safe alternate path exists, record the case and
   continue independent offline workers; do not manufacture a correctness result.
   Runs/resources influenced by fallback are collection evidence, not proof that
   the original shader or game was fixed. Keep ordinary validation/error paths.
5. Fix one shared cause per batch with a synthetic intended RED -> unchanged
   GREEN, affected neighbors and GPU checks when needed. Rerun the affected group,
   then one original game retry. Reassess the old48 groups against fork output;
   do not reimplement fixes already present in this base.
6. Update this plan, test debt and shared handoff after each meaningful checkpoint.
   Commit completed changes separately. No push without current authorization.

## Constraints and recovery

Run one native build/test/game at a time. Check active owned processes before
resuming; prior sessions can leave bounded supervisors alive. Use the local
windows-local.cmd/vcvars64 wrapper, independent dependency worktrees, bounded
hidden workers, drained streams and owned-process cleanup. Preserve old worktrees,
original installed EXE454656, original eboot5178 and user SaveData.

Artifacts under the previous _Build are local evidence and are not in Git.
Do not remove that folder as routine cleanup. Preserve the seed manifests,
captures, private runtimes and compatible caches. Git stores the plan and source;
shared handoffs store the current checkpoint and artifact locations for Windows/WSL.

Issue108 publication is restricted to the user's menu/entry milestones with
accurate tested revision/scope and PR497 linkage. The upstream fork baseline does
not prove the published PR497 head. No new issue comment or push has occurred.
