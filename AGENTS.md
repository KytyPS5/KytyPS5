# KytyPS5 development rules

## Reproduce before fixing

For emulator bug fixes and missing guest functionality, read and apply
[emulator-regression-first](.agents/skills/emulator-regression-first/SKILL.md).

1. Write a minimal regression test before changing production behavior.
2. Run it against the existing implementation and record the expected failure.
   A build failure, missing dependency, or unrelated crash is not a valid reproduction.
3. Fix the underlying emulator mechanism, then run the same test unchanged to prove
   the failure is resolved. Refactor only while preserving that regression.
4. Validate neighboring cases and existing affected tests, then retry the original
   game when execution is authorized. Record commands, revisions and outcomes.

If a GPU reproduction can hang or reset the driver, first make it bounded or reproduce
the defect in decoder, IR, resource planning, or scheduling tests. Do not repeatedly
reset the GPU to obtain a failing test. Read-only diagnosis and temporary diagnostic
instrumentation are allowed before the reproduction; do not present them as a fix.
Documentation and workflow-only changes do not require artificial code tests.

## Fix shared mechanisms across games

- Derive expected behavior from the guest ISA/API/ABI and host API contracts. A game
  log supplies a reproduction, not the specification.
- Implement in the shared decoder, IR, backend, resource model, kernel or scheduler
  responsible for the behavior. Extend existing abstractions when the semantics fit.
- Do not branch on a game title, title ID, shader hash, guest address or installation
  path to make a test pass. Use decoded semantics, explicit metadata and actual device
  capabilities. Keep game-specific captures and launch settings separate from core logic.
- Cover relevant input variations and failure boundaries, not only the first captured
  values. Use reusable, synthetic fixtures that other games can exercise.
- Preserve unsupported-case errors, bounds checks and validation. Do not fabricate
  zero resources, skip required work, suppress errors, or increase limits blindly.
- Run available cross-game cases/corpora affected by the change. If another game is
  unavailable, state that limit and use independent synthetic cases; do not claim
  cross-game compatibility or a working game from a passing shader audit.
- Avoid speculative architecture rewrites: make the verified semantic correction
  general enough for its supported inputs, and expose unsupported capabilities clearly.

## Windows and WSL execution

- Discover the current repository/build paths; do not assume an old drive letter.
- This workspace targets a native Windows emulator. WSL may orchestrate native Windows
  tools; a Linux build does not validate the Windows executable.
- Run one native build at a time and freeze its source inputs until it completes.
- Give subprocesses bounded lifetimes, drain both output streams, and dispose of them.
  Batch workers must not create visible console windows. Stop only task-owned processes.
- Preserve user files, proprietary game data and existing artifacts. Store captures,
  diagnostic logs and build output outside tracked source files.

The versioned skill linked above is the shared source for Windows and WSL. Keep any installed
personal copies synchronized with it when changing this workflow.

## Launch progress record

Maintain [the Ghost of Yōtei launch plan](docs/ghost-of-yotei-windows-plan.md) after
meaningful fixes, shader batch audits and game retries. Record the tested source revision,
artifact/log references, current runtime blocker and next required work. Distinguish build,
CPU audit, GPU regression, rendered frame, menu and gameplay results. Keep untested stages
explicitly pending; group batch failures by shared cause and note overlapping shader counts.

## Publish Ghost of Yōtei status

The user authorizes publishing verified progress updates to
[game issue #108](https://github.com/KytyPS5/KytyPS5/issues/108).
The user defines the only publication milestones as a visually confirmed menu
and, afterwards, confirmed entry into the game. Post a concise checkpoint only
after reaching each of these milestones and link
[fxpw's bring-up PR #497](https://github.com/KytyPS5/KytyPS5/pull/497).
Read the latest issue comments before posting and avoid duplicate updates.

Include the tested revision, validation scope, actual runtime outcome, remaining
blocker and next action. Distinguish local-only commits from the published PR
head and CI; keep rendered pixels, menu and gameplay as separate milestones.
Record the posted comment URL in the launch plan and shared handoff. Keep
intermediate fixes, tests, audits, retries and blockers in local records; they
do not authorize another issue comment. The initial status comment predates
this narrower user instruction; do not repost it or publish a clarification.

## Resume Ghost of Yōtei work

When the user says "continue" or another agent takes over this task, use the
first checkpoint in [the launch plan](docs/ghost-of-yotei-windows-plan.md) as the
current progress record and [the test debt](docs/emulator-test-debt.md) for
unproved cases. Treat both as leads to verify, not as a substitute for current
evidence. Read the shared-context `README.md` and latest KytyPS5 handoff in
`C:\Users\fxpw\.codex\shared-context` (WSL:
`/mnt/c/Users/fxpw/.codex/shared-context`); add a new concise handoff after
meaningful work without replacing older notes.

Before editing, inspect the current branch, commits, `git status` and diff;
preserve all existing work. Check the newest `_Build/runs/*/run.json`, its
`stdout.txt`, `stderr.txt`, `_kyty.txt`, any readback, and the exact installed
executable hash. Check whether the last emulator, test, Ninja or MSBuild process
is still running; a prior chat may have ended while a bounded run continued.
Only stop processes owned by this task. A shown-frame counter, compiled SPIR-V
or successful pipeline does not prove visible pixels or a menu.

For each new blocker, first record the required synthetic regression in the
test debt, then follow the regression-first rules above. Build the native
Windows target through `_Build/windows-local.cmd`, which initializes
`vcvars64.bat`; serialize builds and keep their source inputs fixed. Use bounded
test and game runs, record their artifact paths and process cleanup, and update
the launch plan only with verified results. Commit a completed fix separately.
Push only when the current conversation authorizes it; never force-push to
resolve a conflict. Keep the first nonzero frame, menu and gameplay as separate
milestones.
