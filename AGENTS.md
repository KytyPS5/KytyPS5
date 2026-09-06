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
