# Contributing to KatyPS5

Thank you for your interest in contributing to KatyPS5! This document provides guidelines for
making contributions.

## Types of Contributions

### Bug Reports and Testing

One of the most valuable contributions is testing games and reporting detailed bugs:

- **Search existing issues** before opening a new one
- Use the **Game Emulation Bug Report** template when creating an issue
- **Attach the complete log file** (usually from `%APPDATA%\KatyPS5` on Windows)
- Include the **game name**, **exact error or behavior**, and **steps to reproduce**
- If graphics are broken, include a **screenshot** if possible

### Code Contributions

Code contributions should follow these principles:

#### Before Starting

1. **Check existing issues and pull requests** to avoid duplicating work
2. **For large changes**: Open an issue or discussion first to get feedback
3. **Read [`CLAUDE.md`](CLAUDE.md)** and [`specs/CONSTITUTION.md`](specs/CONSTITUTION.md)
   - Don't break existing functionality
   - Keep semantic versioning in sync (`vX.Y.Z`)
   - Analyze before fixing (avoid regressions)

#### During Development

1. **Create a focused, descriptive branch**
   ```bash
   git checkout -b fix/issue-123-controller-hotplug
   git checkout -b feat/save-state-improvements
   git checkout -b chore/update-dependencies
   ```

2. **Use Conventional Commits** in your commit message:
   - `feat:` new feature
   - `fix:` bug fix
   - `chore:` refactoring, cleanup, dependency updates
   - `docs:` documentation improvements
   - `security:` security fixes or hardening
   - Example: `fix: prevent crash when GPU memory is exhausted`

3. **Code style and formatting**:
   - Set up the clang-format pre-commit hook:
     ```powershell
     python -m pip install pre-commit
     python -m pre_commit install --install-hooks
     ```
   - This automatically formats `.cpp`, `.h`, and `.inc` files in `src/`
   - Alternatively, run manually before committing:
     ```bash
     clang-format -i src/your_file.cpp
     ```

4. **Add comments for non-obvious code**:
   - Explain *why* you made a choice, not *what* the code does
   - For examples, see [`src/launcher/src/main.cpp`](src/launcher/src/main.cpp)
   - Use the [code-annotator skill](\.claude/skills/code-annotator/SKILL.md) if needed

5. **Include tests when practical**:
   - Add or update regression tests in [`tests/`](tests)
   - Use GoogleTest framework (same as existing tests)
   - Run tests locally or verify they pass in CI

#### Platform-Specific Changes

- **Windows** is the primary platform; changes to shared code must not regress it
- **Linux and macOS** changes confined to platform-specific code paths only need to build there
- CI will verify all platforms

### Documentation Improvements

- Improve this README
- Fix typos or clarify confusing sections
- Add architecture or design documentation
- Update CHANGELOG.md when appropriate

## Testing

Before submitting a pull request:

1. **Verify your changes compile**:
   - Use the [build-verifier custom agent](\.claude/agents) if available
   - Or push your branch and monitor the GitHub Actions workflow

2. **Run regression tests** (Windows example; use `_Build/linux` for Linux):
   ```bash
   cmake --build _Build/windows --target katy_tests
   ctest --test-dir _Build/windows --output-on-failure
   ```

3. **Test on the target platform** if you've made platform-specific changes

## Pull Request Process

1. **Update CHANGELOG.md** with a summary of your changes (if not already done)
2. **Write a clear PR title** using a Conventional Commit format
3. **Reference related issues** (e.g., "Closes #123")
4. **Allow CI to run**—all platforms must pass before merging
5. **Respond to review feedback** promptly

## Developer Information

### Key Areas of the Codebase

- **Shader recompilation**: [`src/graphics/shader/recompiler`](src/graphics/shader/recompiler)
  - Instruction decoding, intermediate representation, SPIR-V emission
- **GPU backend**: [`src/graphics/guest_gpu`](src/graphics/guest_gpu) and
  [`src/graphics/host_gpu`](src/graphics/host_gpu)
- **PS5 system emulation**: [`src/emulator`](src/emulator)
- **Launcher GUI**: [`src/launcher`](src/launcher)
- **Tests**: [`tests/`](tests)

### Graphics Architecture

- The PS5 GPU is based on AMD RDNA 2
- The renderer targets **Vulkan 1.3**
- Reference: [RDNA 2 Instruction Set Architecture Reference Guide](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)

### Workflows

This repository uses:

- **Spec-Driven Development** — see [`specs/`](specs) for design documents
- **Conventional Commits** for consistent history
- **Semantic Versioning** for releases
- **Continuous Integration** on Windows, Linux, and macOS

## Code of Conduct

We expect all contributors to be respectful and professional. Harassment, discrimination, or
abusive behavior is not tolerated.

## Questions?

- Open an issue for clarification
- Ask in a discussion thread
- Review [`CLAUDE.md`](CLAUDE.md) for project conventions and policies

Thank you for contributing to KatyPS5!
