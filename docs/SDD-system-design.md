# KatyPS5 System Design Document (SDD)

**Version**: 0.4.0  
**Last Updated**: 2026-11-14  
**Maintained by**: erickson558  
**License**: GPL-2.0-only  

## Purpose

This document is the **single source of truth** for understanding the KatyPS5 fork's architecture, capabilities, roadmap, and decision rationale. Use this with `CONSTITUTION.md` and `CLAUDE.md` to understand what this project is and why changes should follow certain patterns.

---

## 1. Project Overview

### What is KatyPS5?

KatyPS5 is a **PlayStation 5 emulator** written in C++. It is a fork of the upstream emulator [KytyPS5](https://github.com/KytyPS5/KytyPS5), which itself evolved from the original [Kyty](https://github.com/InoriRus/Kyty) (MIT-licensed).

**Current capabilities:**
- x86-64 JIT compilation for PS5 CPU instructions
- Vulkan-based GPU emulation (ray tracing, compute shaders)
- Audio I/O and haptics (DualSense controller)
- Save data persistence (new in v0.4.0)
- Game compatibility: 100+ titles (see upstream database)

**What it is NOT:**
- A finished, production-ready emulator (early beta)
- Capable of running all PS5 games (many still incompatible)
- Legally the same as upstream (fork adds Spanish UI, stricter security)
- Relicenseable to MIT or Apache (GPL-2.0-only is permanent)

### Fork Identity

The fork is named **KatyPS5** (rebranding of KytyPS5 to Katy, as per `specs/001-katyps5-rebrand/`). Key innovations:

1. **SaveDataMemory2**: Atomic persistence, backups, checkpoint survival across restarts
2. **Spanish UI** with Qt translation infrastructure
3. **Spec-Driven Development (SDD)** framework (specs/, CONSTITUTION.md, CLAUDE.md)
4. **Security hardening**: CodeQL, Dependabot, SECURITY.md
5. **Launcher UX**: Control remapping, fullscreen, donation button

---

## 2. Architecture at a Glance

### Layered Stack

```
┌─────────────────────────────────────────┐
│   Qt Launcher (src/launcher/)           │  ← User-facing interface
├─────────────────────────────────────────┤
│   Emulator Core (src/)                  │  ← CPU/GPU/Audio orchestration
├─────────────────────────────────────────┤
│ Kernel (src/kernel/)  ┌─ File System    │  ← OS emulation layer
├─────────────────────┤ └─ Save Data      │
│ Graphics (src/graphics/) - Vulkan       │  ← GPU/shader compilation
├─────────────────────────────────────────┤
│ Libraries (src/libs/) - Audio/Semaphore │  ← PS5 standard library stubs
├─────────────────────────────────────────┤
│ 3rdparty/ (SDL2, FFmpeg, Vulkan, etc)   │  ← External dependencies
└─────────────────────────────────────────┘
```

### Core Components

| Component | Path | Responsibility |
|-----------|------|-----------------|
| **Emulator** | `src/emulator.{cpp,h}` | CPU/GPU/kernel lifecycle, state machine |
| **CPU (MIPS->x86-64 JIT)** | `src/kernel/cpu/` | Instruction translation, register mapping |
| **GPU (Vulkan)** | `src/graphics/` | Command queue submission, shader compilation, memory management |
| **Kernel** | `src/kernel/` | PS5 syscalls (file I/O, threading, events, save data) |
| **Audio** | `src/libs/audio/` | Output buffering, haptics, device clock synchronization |
| **Launcher** | `src/launcher/` | Qt UI, game list, settings, compatibility DB fetch |
| **Save Data** | `src/kernel/saveDataMemory2/` | Persistent storage, atomic writes, recovery |

### Design Principles

1. **No local build**: GitHub Actions is the real compiler. Code must compile on Windows, Linux, macOS without a local toolchain.

2. **Upstream-first**: Bug fixes and optimizations come from KytyPS5/KytyPS5 whenever possible. Fork changes are additive (UI, localization, SDD).

3. **Spec-first**: Any non-trivial change gets a spec/ folder with:
   - `spec.md`: What and why
   - `plan.md`: How (steps, risks, dependencies)
   - `tasks.md`: Concrete git operations and CI gates

4. **Safety before speed**: Never break existing functionality. If unsure, test in CI first.

5. **Comments explain why, not what**: Code should be self-documenting. Comments clarify hidden constraints, workarounds, invariants.

---

## 3. Current State & Roadmap

### Completed Work

#### v0.4.0 (Released 2026-10-13)
- ✅ **SaveDataMemory2 persistence**: Atomic writes, per-slot backups, import support
- ✅ **Spanish UI & translations**: Complete Qt locale system
- ✅ **Security hardening**: CodeQL, Dependabot, command-injection fixes
- ✅ **SDD infrastructure**: Specs, constitution, automated task tracking
- ✅ **Upstream backports**: File seek lock, truncation, event queueing, audio timing

#### v0.3.x (Earlier)
- ✅ KatyPS5 rebranding (complete, case-preserving)
- ✅ Upstream stability fixes
- ✅ Launcher improvements (fullscreen, control remapping)

### High-Priority Upstream Integrations (Not Yet Done)

| Area | Upstream Gap | Effort | Risk | Blocks |
|------|--------------|--------|------|--------|
| **Dependencies** | SDL2→SDL3, FFmpeg updates, Tracy, CMake 3.10+ | Medium | Low | Everything else |
| **Security fixes** | Launcher command-injection (newer PR), Windows MSVC compatibility | Low | Low | Release 0.4.1 |
| **Audio** | MP3 header parsing, popping/time-stretch, Bluetooth haptics | Medium | Medium | Audio features |
| **GPU/Rendering** | Render target layouts, BDA optimization, image page tracking | High | High | Game compatibility |
| **Ray tracing** | Native traversal, wave64 queries, HDR | Very high | Very high | Performance, visual fidelity |
| **Kernel** | Console FP environment, V_PERM_B32, LoadExec (Windows), PlayGo | Medium | Medium | Compatibility |
| **Controllers** | DualSense speaker/triggers, swipe bindings, advanced haptics | Low | Low | User experience |

### Blocked by Dependencies

The **SDL2→SDL3 migration** is the critical path:
- Affects renderer initialization, event loop, input handling
- Blocks all GPU/rendering backports (they assume SDL3)
- Must be done before ray tracing integration

### Risk-Weighted Roadmap (2026 Q4/Q1)

1. **Phase 1 (v0.4.1-0.4.2)**: Dependencies & Security
   - SDL3 migration (create spec, test in CI)
   - MSVC/Windows compatibility
   - Security backports
   - 4-6 weeks, low risk

2. **Phase 2 (v0.5.0)**: Stability & Audio
   - Kernel/filesystem fixes
   - Audio MP3 parsing
   - Event/SaveData robustness
   - 4-8 weeks, medium risk

3. **Phase 3 (v0.6.0)**: GPU/Rendering
   - Layouts, BDA, image tracking
   - Render target optimizations
   - 8-12 weeks, high risk

4. **Phase 4 (v0.7.0+)**: Ray Tracing
   - BVH traversal, wave64, HDR
   - 12+ weeks, very high risk

---

## 4. Directory Structure

```
.
├── .claude/                          # AI-specific configs
│   ├── agents/                       # Custom agents (build-verifier, release-manager, security-auditor)
│   ├── skills/                       # Reusable workflows
│   │   ├── upstream-sync/           # (NEW) Upstream synchronization
│   │   ├── debug-fix-release/       # Analyze→fix→version→commit workflow
│   │   ├── github-publish/          # Push, tag, release
│   │   ├── code-annotator/          # Comment C++ code
│   │   └── security-hardening/      # Static analysis, CodeQL, Dependabot
│   └── README.md
├── .github/
│   ├── workflows/
│   │   ├── build.yml                # CI: compile Windows/Linux/macOS, run tests
│   │   └── codeql.yml               # Security: static analysis
│   ├── dependabot.yml               # Dependency updates
│   └── SECURITY.md                  # Disclosure policy
├── specs/                            # SDD-driven change proposals
│   ├── CONSTITUTION.md              # Non-negotiables
│   ├── TEMPLATE-spec.md             # Template for new specs
│   ├── 001-katyps5-rebrand/         # Product rename (done)
│   ├── 002-upstream-stability/      # Upstream backports (done)
│   └── 003-save-memory-persistence/ # SaveDataMemory2 (done)
├── docs/                             # User-facing docs
│   ├── upstream-review-2026-10-12.md # Detailed comparison with upstream
│   └── save-memory.md               # SaveDataMemory2 user guide
├── src/
│   ├── main.cpp, emulator.cpp       # Entry point, orchestration
│   ├── common/                       # Utility headers (logging, types)
│   ├── graphics/                     # Vulkan backend
│   │   ├── spirvParser/             # SPIR-V disassembly
│   │   ├── converter/               # GPU ISA → SPIR-V
│   │   └── ... (render targets, shaders, memory)
│   ├── kernel/                       # PS5 syscall emulation
│   │   ├── cpu/                      # x86-64 JIT from MIPS
│   │   ├── fs/                       # File system
│   │   ├── saveDataMemory2/         # Persistence (v0.4.0+)
│   │   ├── events/                   # Event queue
│   │   └── ... (IPC, mutexes, threads)
│   ├── libs/                         # PS5 library stubs
│   │   ├── audio/                    # AudioOut2, ATRAC-9
│   │   └── ... (libc, libm, video, others)
│   ├── loader/                       # ELF loader
│   └── launcher/                     # Qt GUI
│       ├── src/compatibilityDatabase.cpp  # (NOTE: uses upstream DB URL)
│       ├── ui/                       # QML/UI files
│       └── translations/             # Spanish .ts files
├── tests/                            # GoogleTest regression suite
│   ├── AudioOut2PortTests.cpp
│   ├── SaveDataMemoryTests.cpp       # (NEW in v0.4.0)
│   └── ... (filesystem, events, memory, shaders)
├── 3rdparty/                         # Vendored dependencies (do NOT hand-edit)
│   ├── SDL2/ (CMakeLists.txt links to pre-built libs)
│   ├── FFmpeg/
│   ├── Vulkan-Headers/
│   ├── VulkanMemoryAllocator/
│   ├── cpuinfo/, fmt/, magic_enum, nlohmann_json, spdlog, stb, tracy, xxHash
│   └── ... (platform-specific: winpthread on Windows)
├── CHANGELOG.md                      # Version history (semver)
├── CMakeLists.txt                    # Build config (v0.4.0 hardcoded)
├── CLAUDE.md                         # AI agent guidance (no local build, SDD, conventions)
├── LICENSE                           # GPL-2.0-only
├── README.md                         # User-facing overview
└── SECURITY.md                       # Vulnerability disclosure policy
```

**Key file paths to remember:**
- Version: `CMakeLists.txt:123`, `CHANGELOG.md:7`
- Rebranding exceptions: `CLAUDE.md` "Naming note" section
- Upstream compatibility DB: `src/launcher/src/compatibilityDatabase.cpp` (hardcoded URL)
- CONSTITUTION: `specs/CONSTITUTION.md` (precedence over convenience)

---

## 5. Build & CI/CD Pipeline

### No Local Compilation

**Critical fact**: This environment has no CMake, Ninja, Qt6, or Vulkan SDK. All compilation happens in GitHub Actions.

### CI Pipeline (.github/workflows/)

**build.yml** (on every push, PR, schedule):
1. Matrix: Windows (MSVC), Linux (gcc), macOS (clang)
2. Install deps (Qt6, Vulkan SDK, CMake, Ninja)
3. Configure & build (cmake + ninja)
4. Run regression tests (ctest -T test)
5. Upload test logs, build artifacts
6. Publish release packages (if main + tag)

**codeql.yml** (weekly + manual):
- Static analysis for security vulnerabilities
- Tracks C++ pointer/buffer issues, SQL injection, command-injection

### How to Test Locally (for human contributors)

```bash
# On Windows with MSVC + Qt6 + Vulkan SDK installed:
cmake -B _build -S . -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja -C _build
ctest --test-dir _build -VV

# Same on Linux/macOS with appropriate compiler
```

### How to Test as AI (in this environment)

1. **Static checks**: grep, view, understand code flow
2. **CI validation**: `gh run watch` after pushing to a branch
3. **Code review**: Use security-auditor, code-review agents
4. **Build verification**: Use build-verifier agent

---

## 6. Key Workflows

### Workflow 1: Bug Fix (with Spec)

```
1. Identify bug, run security-auditor or code-review agent
2. Create specs/NNN-slug/
   - spec.md: root cause, impact, risk
   - plan.md: fix steps
   - tasks.md: git operations
3. Implement fix (add comment explaining why, not what)
4. Push to fix/NNN-slug branch
5. Watch CI with `gh run watch`
6. If green: merge via release-manager (version bump, tag, release)
```

### Workflow 2: Feature Addition

```
1. Create specs/NNN-slug/ (spec-first)
2. Create branch feat/NNN-slug
3. Implement with comments, tests, and .ts translation files if UI
4. Push and validate in CI
5. Merge and release via release-manager
```

### Workflow 3: Upstream Backport (Major)

```
1. Use explore agent to analyze upstream changes
2. Create specs/NNN-upstream-PHASE/ for each phase
3. For each phase:
   a. Create branch sync/NNN-upstream-PHASE
   b. Implement changes (preserve fork's innovations)
   c. Push, validate in CI
   d. Merge via release-manager
4. Update CHANGELOG.md with upstream credits
5. Bump version (usually minor)
6. Tag and publish
```

### Workflow 4: Security Incident Response

```
1. Run security-auditor agent (immediate)
2. Create specs/NNN-security-INCIDENT/
3. Implement fix with `security:` prefix
4. Document in SECURITY.md
5. Merge to main ASAP (out-of-band release OK)
6. Publish advisory
```

---

## 7. Dependency Graph

### Build-Time

```
CMakeLists.txt
  ├── 3rdparty/CMakeLists.txt
  │   ├── SDL2 (windows: link to pre-built)
  │   ├── FFmpeg (windows: link to pre-built)
  │   ├── cpuinfo (compile if not vendored)
  │   ├── fmt, nlohmann_json, spdlog, xxHash (header-only or compile)
  │   ├── Vulkan-Headers, VulkanMemoryAllocator (header-only)
  │   ├── SPIRV-Headers, SPIRV-Tools (compile for validation)
  │   └── Tracy (optional, performance profiling)
  ├── Qt6 (external, cmake -DCMAKE_PREFIX_PATH=$Qt6_ROOT)
  ├── Vulkan SDK (external, vkEnumerateInstanceVersion, etc)
  └── glslangValidator (external tool, required for shader compilation)
```

### Runtime

```
kyty_emulator (executable)
  ├── SDL2 (windowing, input, audio)
  ├── Vulkan (GPU communication)
  ├── FFmpeg (video codecs)
  ├── cpuinfo (CPU capability detection)
  └── Tracy (optional profiling)

katy_emulator / launcher (Qt executable)
  ├── Qt6 (GUI framework)
  ├── kyty_emulator (emulator core)
  └── (network: curl for compatibility DB fetch)
```

### Test-Time

```
katy_tests (GoogleTest)
  ├── kyty_emulator core
  └── gtest (Google Test framework, vendored)
```

---

## 8. Known Limitations & Constraints

### Build Toolchain

| Platform | Compiler | Status |
|----------|----------|--------|
| Windows | MSVC 14.x (Visual Studio 2022) | ✅ Primary target |
| Linux | GCC 11+ | ✅ Full support |
| macOS | Clang 14+ (Xcode) | ✅ Full support |

All require: CMake 3.10+, Ninja, Qt6.4+, Vulkan SDK 1.3.x

### AI Agent Limitations

- **No local build**: Cannot compile or run emulator locally. GitHub Actions is the only compiler.
- **No Vulkan SDK**: Cannot validate GPU code without CI.
- **No Qt6**: Cannot inspect .ui files graphically; must read XML.
- **No GPU**: Cannot test rendering; must read code and trust CI.
- **No PlayStation 5**: Cannot test actual game compatibility (use upstream's game list).

### Scope Constraints

- **GPL-2.0-only**: Cannot relicense to MIT/Apache without upstream consent.
- **Upstream compatibility**: Fork changes must not regress upstream compatibility.
- **Naming**: `Kyty` in LICENSES/Kyty-MIT.txt, compatibility DB URL, and README credits stay unchanged.
- **Semver**: Version must match across CMakeLists.txt, CHANGELOG.md, git tags, GitHub Releases.

---

## 9. Testing Strategy

### Test Coverage

| Test Suite | Location | Runs | Notes |
|-----------|----------|------|-------|
| **Filesystem** | `tests/KernelFileSystemTests.cpp` | CI (all platforms) | Seek lock, truncation, descriptor reuse |
| **Save Data** | `tests/SaveDataMemoryTests.cpp` | CI (all platforms) | Restart survival, atomicity, backups |
| **Audio** | `tests/AudioOut2*.cpp` | CI (all platforms) | Timing, pacing, haptics |
| **Events** | `tests/EventQueueLifetimeTests.cpp` | CI (all platforms) | Queueing, persistence, thread safety |
| **Memory** | `tests/VirtualMemoryAllocationTests.cpp` | CI (all platforms) | Allocation, tracking, paging |
| **Shaders** | `tests/ShaderRecompilerComputeTests.cpp` | CI (all platforms) | SPIR-V translation, compute |

### Running Tests Locally

```bash
# Configure & build
cmake -B _build -S . -DCMAKE_BUILD_TYPE=Debug
ninja -C _build

# Run all tests with CTest
ctest --test-dir _build -VV

# Or run specific suite
./katy_tests  # All GoogleTest suites
```

### CI Test Validation

- **Timeout**: 60 seconds per test (exposes deadlocks)
- **Platforms**: Windows (MSVC), Linux (GCC), macOS (Clang)
- **Coverage**: All test suites run on all platforms
- **Failure handling**: Failed tests block merge (GitHub branch protection)

---

## 10. Release Process

### Versioning (Semantic)

- **MAJOR** (v1.0.0 → v2.0.0): Breaking changes (save format, CLI, config incompatible)
- **MINOR** (v0.4.0 → v0.5.0): New features, backward compatible
- **PATCH** (v0.4.0 → v0.4.1): Bug fixes, no new features

**Constraint**: Version must match everywhere:
- `CMakeLists.txt`: `project(katyps5 ... VERSION X.Y.Z)`
- `CHANGELOG.md`: `## [X.Y.Z] - YYYY-MM-DD`
- Git tag: `vX.Y.Z`
- GitHub Release: Title must be `vX.Y.Z`

### Release Checklist

```
1. Ensure main is green (CI passed)
2. Update CHANGELOG.md with new entry
3. Bump version in CMakeLists.txt
4. Commit: `chore: release vX.Y.Z`
5. Tag: `git tag -a vX.Y.Z -m "Release vX.Y.Z"`
6. Push: `git push origin main vX.Y.Z`
7. GitHub automatically creates release + builds artifacts
8. Verify release packages are present
```

### Use release-manager Skill

The skill automates steps 3-8:

```bash
# From any branch
# Simply tell release-manager to bump version and release
# It will:
# - Verify main is green
# - Bump version
# - Update CHANGELOG
# - Commit + tag
# - Push + publish release
```

---

## 11. Security & Hardening

### Security Policy (SECURITY.md)

1. **Report confidentially**: security@katyps5.example (adjust to real email)
2. **No public disclosure** until fix is released
3. **Patching**: Security issues get patch releases (vX.Y.Z+1)

### Static Analysis

**CodeQL** (weekly + manual):
- Scans for pointer/buffer overflows
- Detects SQL injection (N/A, no SQL here)
- Flags unsafe C++ patterns

**Dependabot** (weekly):
- Checks for outdated dependencies
- Proposes updates, creates PRs
- Fork maintainer reviews + merges

**Manual audits**:
- Use security-auditor agent for deep reviews
- Run `cppcheck` locally (if installed)
- Peer review before merge

### Common Attack Vectors in Emulators

| Vector | Mitigation |
|--------|-----------|
| **Buffer overflow** | Bounds checking on all inputs (GPU commands, save data, files) |
| **Command injection** | Escape shell arguments; use argv arrays (already fixed in fork) |
| **Path traversal** | Validate file paths against sandbox root |
| **Memory corruption** | Use RAII, smart pointers; avoid raw new/delete |
| **Timing attacks** | Avoid timing-dependent branches in crypto (not applicable; no crypto here) |

---

## 12. Extending the Fork

### Adding a New Feature

1. **Write a spec** in `specs/NNN-feature-name/`
   ```
   NNN/
   ├── spec.md       (what + why + constraints)
   ├── plan.md       (steps, dependencies, risks)
   └── tasks.md      (concrete git operations)
   ```

2. **Choose the branch type**:
   - `feat/NNN-*` for new functionality
   - `fix/NNN-*` for bug fixes
   - `sync/NNN-*` for upstream backports
   - `docs/NNN-*` for documentation only

3. **Implement**:
   - Follow existing code style (no specific enforcer; grep existing code)
   - Add comments explaining *why*, not *what*
   - If UI: update `src/launcher/translations/` `.ts` files
   - If kernel/GPU: add tests to `tests/`
   - Use Conventional Commits: `feat:`, `fix:`, `chore:`, `docs:`, `security:`

4. **Test in CI**:
   - Push to branch
   - Watch `gh run watch` for Windows/Linux/macOS results
   - Ensure regression tests pass

5. **Release**:
   - Use release-manager skill
   - Bump version, tag, publish
   - Update CHANGELOG.md

### Adding a Translation

1. Update `src/launcher/translations/katyps5_es.ts` (or create new .ts for other lang)
2. Rebuild launcher (CI does this)
3. Qt localization system automatically loads correct .qm files

### Backporting Upstream Changes

See `.claude/skills/upstream-sync/SKILL.md` for detailed workflow.

---

## 13. Glossary & Key Terms

| Term | Meaning |
|------|---------|
| **Emulator** | Software that mimics a PlayStation 5's hardware (CPU, GPU, audio) |
| **JIT** | Just-In-Time compilation: translates PS5 CPU instructions (MIPS) to x86-64 at runtime |
| **Vulkan** | Graphics API for GPU communication (cross-platform: Windows, Linux, macOS) |
| **SPIR-V** | Intermediate representation for GPU shaders (assembly-like) |
| **Syscall** | System call; emulated PS5 kernel API (file I/O, threading, etc.) |
| **Backport** | Cherry-picking upstream changes into the fork |
| **Regression** | Breaking existing functionality that previously worked |
| **SDD** | Spec-Driven Development; using specs/ to plan non-trivial changes |
| **CI/CD** | Continuous Integration/Continuous Deployment (GitHub Actions here) |
| **Semver** | Semantic Versioning (vMAJOR.MINOR.PATCH) |

---

## 14. References & Links

**External**:
- [KytyPS5 upstream](https://github.com/KytyPS5/KytyPS5)
- [Original Kyty](https://github.com/InoriRus/Kyty)
- [PlayStation 5 Specs](https://www.playstation.com/en-us/ps5/specs/)
- [Vulkan Documentation](https://www.vulkan.org/learn)
- [Qt6 Documentation](https://doc.qt.io/qt-6/)
- [CMake 3.10 Guide](https://cmake.org/cmake/help/v3.10/)

**Internal**:
- [specs/CONSTITUTION.md](./specs/CONSTITUTION.md) — Non-negotiables
- [CLAUDE.md](./CLAUDE.md) — AI agent guidance
- [docs/upstream-review-2026-10-12.md](./docs/upstream-review-2026-10-12.md) — Detailed comparison
- [.claude/skills/upstream-sync/SKILL.md](./.claude/skills/upstream-sync/SKILL.md) — Upstream sync workflow
- [.claude/skills/debug-fix-release/SKILL.md](./.claude/skills/debug-fix-release/SKILL.md) — Bug fix workflow
- [.claude/skills/github-publish/SKILL.md](./.claude/skills/github-publish/SKILL.md) — Release workflow

---

**Document Metadata**:
- **Format**: Markdown (render-friendly on GitHub)
- **Audience**: Humans, AI agents, and anyone trying to understand KatyPS5
- **Maintenance**: Updated whenever a new major feature or refactor lands
- **License**: Same as project (GPL-2.0-only)

---

*Last updated: 2026-11-14 by AI agent (erickson558 fork owner)*  
*Next review: After v0.5.0 release or upstream major sync*
