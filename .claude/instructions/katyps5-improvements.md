---
name: katyps5-improvements
description: |
  Use when: planning improvements to KatyPS5, analyzing features to add, 
  deciding what to work on next, or reviewing roadmap priorities.
  Example: "qué podemos mejorar ahora" o "analiza oportunidades de mejora"
applyTo: "**"
---

# KatyPS5 Improvements & Roadmap Navigator

This instruction guides AI agents in planning, prioritizing, and executing improvements to the KatyPS5 fork.

## Quick Commands You Can Use

When you have a simple request, these one-line prompts will activate the appropriate workflow:

- **"sincroniza mejoras del upstream"** → Use `upstream-sync` skill
- **"qué ha mejorado KytyPS5 que no tenemos aqui"** → Explore agent analyzes differences
- **"arregla este bug sin romper funcionalidad"** → Use `debug-fix-release` skill + security-auditor
- **"publica esta versión"** → Use `release-manager` skill
- **"qué podemos mejorar"** → Suggest from high-priority roadmap
- **"crea una especificación para X"** → Template from specs/TEMPLATE-spec.md
- **"revisa seguridad en este diff"** → Use security-auditor agent

## Before You Start: Read Three Files

1. **[CONSTITUTION.md](../specs/CONSTITUTION.md)** (5 min)
   - Non-negotiables that override convenience
   - License, versioning, functionality guarantees

2. **[CLAUDE.md](../CLAUDE.md)** (5 min)
   - No local build toolchain; use CI
   - Naming exceptions (Kyty vs Katy)
   - Project structure and where things live

3. **[docs/SDD-system-design.md](./SDD-system-design.md)** (20 min)
   - Architecture, roadmap, build pipeline
   - Testing, release process, extending guidelines
   - Use this as your "single source of truth"

## High-Priority Improvements (Ranked by Impact + Feasibility)

### 1. SDL2 → SDL3 Migration (v0.4.1)

**Why**: Blocks GPU/rendering backports from upstream; dependencies upstream uses  
**Effort**: Medium (3-4 weeks)  
**Risk**: Low (well-isolated, good test coverage)  
**What to do**:
- Create `specs/NNN-sdl3-migration/`
- Find all SDL2 #includes and API calls (grep `#include <SDL` and `SDL_` functions)
- Update CMakeLists.txt (3rdparty/ and link targets)
- Update launcher event loop (src/launcher/src/)
- Update renderer (src/graphics/)
- Test in CI: Windows, Linux, macOS
- Use **build-verifier** after each file change

### 2. MSVC/Windows Compatibility (v0.4.1)

**Why**: Security and stability; upstream fixed several Windows-only bugs  
**Effort**: Low (1-2 weeks)  
**Risk**: Low  
**What to do**:
- Create `specs/NNN-windows-compat/`
- Review upstream PR #2229 (macro `near` conflict)
- Review upstream PR #2165 (STL 14.51 compatibility)
- Apply fixes to fork
- Test in CI on Windows
- Use **security-auditor** to validate memory/pointer changes

### 3. GPU Render Target Optimization (v0.5.0)

**Why**: Improves compatibility, game visual quality; moderate upstream diff  
**Effort**: High (6-8 weeks)  
**Risk**: High (core rendering path)  
**What to do**:
- Create `specs/NNN-gpu-render-targets/`
- Study upstream commits `5940e623` (10-11-11 storage) and `5559cfb` (tiling)
- Identify which changes are independent vs interdependent
- Implement in phases: layouts → readbacks → storage formats
- Add regression tests as needed
- Use **code-review** agent to validate each phase
- Use **build-verifier** for CI validation

### 4. Audio MP3 Parsing (v0.5.0)

**Why**: Unlocks MP3 soundtrack support in some games  
**Effort**: Medium (2-3 weeks)  
**Risk**: Medium (audio is tricky; needs testing)  
**What to do**:
- Create `specs/NNN-audio-mp3-parsing/`
- Review upstream commit for `sceAjmDecMp3ParseFrame` implementation
- Implement header parsing (frame length calculation, bitrate, etc.)
- Add tests to `tests/AudioOut2*.cpp`
- Verify no popping/underrun regressions
- Use **build-verifier** and run audio tests on CI

### 5. SaveDataMemory2 Compatibility (v0.5.0)

**Why**: Ensure new kernel changes don't break persistence layer  
**Effort**: Low (1 week, already implemented)  
**Risk**: Low (isolated component)  
**What to do**:
- After each major upstream kernel backport, rerun `SaveDataMemoryTests.cpp`
- Verify restart survival, atomicity, backup restoration
- Add new test cases if upstream introduced new scenarios
- Use **build-verifier** to validate tests pass

### 6. DualSense Haptics & Speaker (v0.6.0)

**Why**: Better controller feedback; upstream has full implementation  
**Effort**: Medium (2-3 weeks)  
**Risk**: Medium (new features; test with real controllers)  
**What to do**:
- Create `specs/NNN-dualsense-haptics/`
- Study upstream PR #2116 (light bar), #2175 (swipes)
- Implement DualSense speaker support (audio routing)
- Implement trigger vibration (haptic feedback)
- Implement touchpad swipe bindings
- Test in CI (no real hardware available; test code paths)
- Update launcher UI if needed

### 7. Ray Tracing Integration (v0.7.0+)

**Why**: Major visual fidelity improvement; complex upstream diff  
**Effort**: Very high (12+ weeks)  
**Risk**: Very high (massive diff; rewrite of GPU subsystem)  
**What to do**:
- **DO NOT START** until SDL3 and GPU rendering backports are complete
- Create `specs/NNN-ray-tracing/`
- Break into sub-phases:
  1. BVH structure validation
  2. Ray traversal kernels
  3. Wave64 queries
  4. HDR integration
- Study each phase in upstream independently
- Test each phase extensively in CI before moving to next
- Use **code-review** agent heavily

## Improvement Selection Matrix

Use this matrix to decide what to work on next:

| Improvement | Impact | Effort | Risk | Blocks | Recommend When |
|-------------|--------|--------|------|--------|-----------------|
| SDL3 migration | High | Medium | Low | GPU backports | Start now (Phase 1) |
| Windows compat | Medium | Low | Low | None | After SDL3 (Phase 1) |
| GPU render targets | Very High | High | High | Ray tracing | After dependencies (Phase 3) |
| MP3 audio | Medium | Medium | Medium | None | Parallel to GPU (Phase 2-3) |
| SaveData compat | Medium | Low | Low | None | After each kernel change |
| DualSense haptics | Medium | Medium | Medium | None | After stability (Phase 2-3) |
| Ray tracing | Very High | Very High | Very High | Nothing | Last (Phase 4+) |

## Workflow Templates

### Template 1: Quick Fix (Typo, small bug)

```
1. Grep for the issue: grep -n "pattern" src/
2. View the file: view path/to/file.cpp:123-150
3. Edit: one simple replace
4. Verify: grep to check fix
5. Commit: "fix: description"
```

No spec needed for truly trivial changes.

### Template 2: Feature or Medium Fix

```
1. Create specs/NNN-slug/
   - spec.md: what, why, constraints
   - plan.md: steps, dependencies, risks
   - tasks.md: git operations
2. Create branch: git checkout -b feat/NNN-slug
3. Implement (add comments explaining why)
4. Test in CI (push, watch gh run)
5. Merge via release-manager (version bump, tag, release)
```

### Template 3: Upstream Backport (Major)

```
1. Use explore agent to analyze upstream changes
2. Create specs/NNN-upstream-PHASE/
3. For each phase:
   a. Create branch sync/NNN-upstream-PHASE
   b. Implement
   c. Validate in CI
   d. Merge via release-manager
4. Update CHANGELOG with upstream credits
5. Bump version (usually minor)
```

### Template 4: Security Fix (URGENT)

```
1. Run security-auditor agent immediately
2. Create specs/NNN-security-INCIDENT/
3. Implement with security: commit prefix
4. Merge to main ASAP
5. Use release-manager for patch release (vX.Y.Z+1)
6. Document in SECURITY.md
```

## Files to Consult

| File | When | Duration |
|------|------|----------|
| `CONSTITUTION.md` | Before ANY change | 5 min |
| `CLAUDE.md` | Before starting work | 5 min |
| `SDD-system-design.md` | Understanding architecture/roadmap | 20 min |
| `CHANGELOG.md` | Versioning decisions | 2 min |
| `specs/TEMPLATE-spec.md` | Creating new spec | 5 min |
| `docs/upstream-review-2026-10-12.md` | Comparing fork vs upstream | 10 min |
| `README.md` | User-facing intro | 2 min |
| `.claude/skills/upstream-sync/SKILL.md` | Sync decisions | 10 min |
| `.claude/skills/debug-fix-release/SKILL.md` | Bug fix workflow | 5 min |
| `.claude/skills/github-publish/SKILL.md` | Publishing workflow | 5 min |
| `SECURITY.md` | Security policy | 2 min |

## Tools to Use

| Task | Tool | When |
|------|------|------|
| Analyze upstream diffs | explore agent | Compare changes |
| Review code for bugs | code-review agent | Before merge |
| Check for vulnerabilities | security-auditor agent | Security changes |
| Validate CI after change | build-verifier agent | Blocking issue? |
| Publish changes | release-manager skill | After merge |
| Annotate C++ code | code-annotator skill | Complex logic |
| Sync upstream | upstream-sync skill | Backport work |

## Key Constraints to Never Break

1. **CONSTITUTION.md §1**: Never break existing functionality
2. **CONSTITUTION.md §2**: License stays GPL-2.0-only
3. **CONSTITUTION.md §3**: Version must match everywhere
4. **CONSTITUTION.md §7**: Spec-first for non-trivial changes
5. **No local build**: Always test in CI before declaring done
6. **Naming exceptions**: Kyty in LICENSES/Kyty-MIT.txt, DB URL, README
7. **SaveDataMemory2**: Don't lose checkpoint persistence
8. **Spanish UI**: Don't break translations
9. **SDD structure**: Keep specs/, constitution, CLAUDE.md alive

## Example Improvements to Pitch

If you get stuck deciding what to improve next, suggest one of these:

1. **"La sincronización con upstream está 1,870 commits atrás. Vamos a migrar SDL2→SDL3 primero"**
   → Start SDL3 migration spec/NNN, use upstream-sync skill

2. **"El fork tiene SaveDataMemory2 que upstream no tiene. Asegurémonos que funciona con los cambios del kernel"**
   → Run SaveDataMemoryTests.cpp in CI, ensure regression tests stay green

3. **"Hay vulnerabilidades de command-injection en el launcher. Reviso las fixes del upstream"**
   → Use security-auditor agent to compare implementations

4. **"Ray tracing es lo próximo grande. Pero necesitamos GPU rendering estable primero"**
   → Create roadmap spec showing dependencies, link from SDD-system-design.md

---

## Summary

**To improve KatyPS5 effectively:**

1. ✅ **Read the constitution** (non-negotiables)
2. ✅ **Understand the architecture** (SDD)
3. ✅ **Pick from the roadmap** (high-priority matrix)
4. ✅ **Use the right skill/agent** (workflow templates)
5. ✅ **Test in CI** (never assume local changes work)
6. ✅ **Release properly** (versioning, tagging, publishing)

---

*This instruction was created: 2026-11-14*  
*Last reviewed: 2026-11-14*  
*Maintained by: erickson558 (fork owner)*
