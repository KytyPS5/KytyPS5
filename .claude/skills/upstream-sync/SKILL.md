---
name: upstream-sync
description: |
  Use when: reviewing upstream KytyPS5 improvements, syncing fork with latest changes, 
  planning feature backports, or analyzing what KatyPS5 can learn from upstream.
  Example: "sincroniza mejoras del upstream" o "qué cambios ha hecho KytyPS5 que no tenemos aqui"
---

# KatyPS5 Upstream Synchronization Skill

Workflow for keeping `erickson558/KatyPS5` in sync with improvements and fixes from upstream `KytyPS5/KytyPS5`.

## What to Do

Use this skill when:
- Reviewing upstream changes and identifying what should be backported
- Planning synchronization of security fixes, stability improvements, or new features
- Comparing fork vs upstream capabilities
- Prioritizing feature implementation

## Quick Reference: Fork vs Upstream Status

**Fork version**: 0.4.0 (as of 2026-10-13)  
**Upstream ahead by**: ~1,870 commits  
**Fork ahead by**: 19 commits (rebranding, specs, SaveDataMemory persistence, Spanish UI, security hardening)

### Key Differences

#### Upstream has (fork doesn't)
- **Ray tracing**: Native BVH traversal, wave64 queries, HDR support
- **GPU**: Optimized render targets (10-11-11), memory layouts, image page tracking
- **Shaders**: V_PERM_B32 translation, indirect descriptors, shared predicate preservation
- **Kernel/Audio**: MP3 header parsing, Bluetooth haptics, DualSense speaker/triggers
- **Launcher**: Self-updater, network toggle, overlay for recording/analysis
- **Dependencies**: SDL3 (vs SDL2), imgui, updated FFmpeg/Tracy
- **Console environment**: FP mode for threads/fibers, LoadExec on Windows

#### Fork has (upstream doesn't)
- **SaveDataMemory2** atomic persistence with backups and restart survival
- **KatyPS5 rebranding** (complete case-preserving substitution)
- **Spanish translations** with Qt locale system
- **SDD infrastructure**: specs/, constitution, plans, tasks
- **Security hardening**: CodeQL, Dependabot, SECURITY.md, command-injection fixes
- **Launcher UX**: Donation button, fullscreen mode, control remapping
- **Specs**:
  - `001-katyps5-rebrand`: Complete product rename
  - `002-upstream-stability`: Backported upstream fixes
  - `003-save-memory-persistence`: SaveDataMemory2 implementation

## Synchronization Strategy

### Phase 1: Dependencies & Build (LOW RISK)
1. **SDL2 → SDL3 migration**
   - Search for all SDL2 includes/functions used
   - Update CMakeLists.txt linking
   - Test on Windows/Linux/macOS via CI

2. **Update other deps** (FFmpeg, Tracy, cpuinfo, glslang)
   - Preserve vendored paths in 3rdparty/
   - Check CMake 3.10+ compatibility
   - Update .github/workflows/build.yml

3. **MSVC/Windows fixes**
   - Macros (`near`), STL 14.51 compatibility
   - Test in CI before merge

### Phase 2: Security & Stability (MEDIUM RISK)
1. **Launcher security**
   - Compare command-injection fixes with upstream (PRs #2210, #2192)
   - Ensure fork's implementation is at least as strong

2. **File/SaveData/Kernel fixes**
   - Seek lock release (upstream b9aa0dc3)
   - Read-only truncation descriptors (0f3674f5)
   - Persistent event queuing (afbb322b)
   - Compatible HTTP/network code

3. **Audio & I/O**
   - MP3 header parsing for sceAjmDecMp3ParseFrame
   - Time-stretching and popping fixes
   - Ensure AudioOut2 clock handling stays robust

### Phase 3: GPU/Rendering (HIGH RISK)
1. **Layouts & memory tracking**
   - Image page table optimization
   - BDA memory improvements
   - Render target layout reuse

2. **Render targets & aliasing**
   - 3D render target fixes
   - 10-11-11 storage format support
   - Tiling optimizations

3. **Ray tracing/BVH** (last - biggest diff)
   - Native traversal
   - Wave64 queries
   - HDR support

**Backport strategy**: DO NOT cherry-pick 1,870 commits individually. Block by feature, test in CI, merge block by block.

### Phase 4: Kernel, Shaders, Controllers (MEDIUM RISK)
- Console FP environment for threads/fibers
- V_PERM_B32 translation
- Indirect descriptor image gathers
- DualSense speaker, triggers, Bluetooth haptics
- Touchpad swipe bindings (PR #2175)
- LoadExec on Windows, PlayGo chunk reading

### Phase 5: Reaply Fork Innovations (CRITICAL - DO NOT LOSE)
1. **SaveDataMemory2 persistence**
   - Verify compatibility with new kernel code
   - Test on top of upstream base

2. **Rebranding**
   - Ensure all `Kyty` → `Katy` substitutions stay (except exceptions in CLAUDE.md)
   - Update launcher, version strings, CMakeLists

3. **Spanish UI & translations**
   - Verify Qt translation system not broken by upstream changes

4. **SDD infrastructure**
   - Keep specs/, CONSTITUTION.md, CLAUDE.md intact
   - Add new specs/ as needed for backport work

5. **Security infrastructure**
   - Maintain CodeQL, Dependabot, SECURITY.md
   - Update GitHub Actions workflows from upstream if better

## How to Execute

### For Each Phase:

1. **Create a feature branch**
   ```bash
   git checkout -b sync/002-upstream-<phase>
   ```

2. **Create or update a spec** in `specs/NNN-upstream-<phase>/`
   - spec.md: what needs to sync
   - plan.md: order of operations
   - tasks.md: git tasks and CI gates
   - See `specs/002-upstream-stability/` as example

3. **Implement changes**
   - Prefer upstream code patterns for compatibility
   - Add comments explaining fork-specific changes
   - Use Conventional Commits: `upstream-sync: <message>`

4. **Validate in CI**
   - Push to branch and watch `.github/workflows/build.yml`
   - Test on Windows, Linux, macOS
   - Run regression tests (`katy_tests`)

5. **Merge to main**
   - Use `release-manager` skill to version, tag, and release
   - Update CHANGELOG.md with phase summary
   - Bump version per semver (usually minor)

## Tools to Use

- **explore agent** - Analyze upstream changes, compare commits
- **security-auditor** - Review upstream security fixes
- **code-review agent** - Validate backports before merge
- **build-verifier** - Test CI after each phase
- **release-manager** - Version, tag, publish (at end of each phase or complete sync)

## References

- **Upstream repo**: https://github.com/KytyPS5/KytyPS5
- **Fork repo**: https://github.com/erickson558/KatyPS5
- **CONSTITUTION.md**: Non-negotiable principles (never break, GPL-2.0-only, semver aligned)
- **CLAUDE.md**: Project structure, tool unavailability, naming conventions
- **Spec examples**: specs/002-upstream-stability/ (already backported fixes)

## Known Conflicts

1. **SDL2 → SDL3**
   - Breaking API changes; affects render initialization, event handling
   - Requires downstream changes in launcher and renderer

2. **Kyty* → Katy* naming**
   - Upstream uses `Kyty*`; fork uses `Katy*`
   - Exceptions: LICENSES/Kyty-MIT.txt, compatibility DB URL, upstream credits
   - See CLAUDE.md for details

3. **CMakeLists.txt divergence**
   - Upstream added imgui, renderdoc_app.h, updated deps
   - Fork may have added custom targets or flags
   - Merge carefully, preserving fork's custom additions

4. **GitHub Actions workflows**
   - Fork has CodeQL, Dependabot, documentation-only skips
   - Upstream may have changed build triggers
   - Merge, then test full CI run

5. **Large GPU/shader diffs**
   - Upstream rewrote many internal GPU APIs
   - Selective backport needed; full merge risk is very high
   - Do in phases, validate at each step

## Success Criteria

- [ ] Dependencies updated (SDL3, FFmpeg, Tracy, etc.)
- [ ] Fork still compiles on Windows, Linux, macOS (CI green)
- [ ] All regression tests pass (katy_tests on all platforms)
- [ ] Security fixes from upstream applied
- [ ] SaveDataMemory2 still works (test in spec 003)
- [ ] Spanish UI not broken
- [ ] SDD structure preserved
- [ ] CHANGELOG.md updated with upstream changes
- [ ] Version bumped and tagged per CONSTITUTION.md §3
- [ ] New GitHub release published

## Next Steps

1. Read `docs/upstream-review-2026-10-12.md` (comparison already done)
2. Create spec/NNN-upstream-dependencies/ for SDL3 migration
3. Create spec/NNN-upstream-security/ for security backports
4. Proceed in phases, merging each to main before starting next
5. Save entire analysis in session files for reference

---

*Last updated: 2026-11-14*  
*This skill documents the 1,870-commit gap analysis and is maintained per CONSTITUTION.md §7 (Spec-first for anything non-trivial).*
