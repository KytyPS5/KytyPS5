# GitHub Actions Version Research for KytyPS5 Workflow

Research conducted: 2026-09-20  
Source: Official GitHub repositories, releases pages, and GitHub Marketplace

---

## 1. `ilammy/msvc-dev-cmd` (currently `@v1`)

### Latest Stable Version
- **Version**: `v1.13.0` (tag: `v1.13.0`)
- **Released**: November 6, 2024
- **Major tag**: `@v1` (points to latest v1.x)

### Changes in v1.13.0
- Added `BuildTools` edition of Visual Studio to search path ([#69](https://github.com/ilammy/msvc-dev-cmd/pull/69))
- Action now usable as a library ([#70](https://github.com/ilammy/msvc-dev-cmd/pull/70))
- Updated to Node.js v20 ([#72](https://github.com/ilammy/msvc-dev-cmd/pull/72))
- Updated `@actions/checkout` to v4 ([#71](https://github.com/ilammy/msvc-dev-cmd/pull/71))

### Breaking Changes / Migration Notes
⚠️ **Critical**: Node.js 20 deprecation on GitHub Actions runners
- GitHub will force Node.js 24 by default starting **June 2, 2026** ([GitHub Blog](https://github.blog/changelog/2025-09-19-deprecation-of-node-20-on-github-actions-runners/))
- Open PR [#106](https://github.com/ilammy/msvc-dev-cmd/pull/106) updates to Node.js 24
- Issue [#99](https://github.com/ilammy/msvc-dev-cmd/issues/99) tracks this deprecation
- **Action required**: Monitor for v1.14.0+ with Node.js 24 support or pin to commit with fix

### Official Sources
- Releases: https://github.com/ilammy/msvc-dev-cmd/releases
- Repository: https://github.com/ilammy/msvc-dev-cmd
- Marketplace: https://github.com/marketplace/actions/enable-developer-command-prompt

---

## 2. `seanmiddleditch/gha-setup-ninja` (currently `@v5`)

### Latest Stable Version
- **Version**: `v6` (tag: `v6`)
- **Released**: February 1, 2025
- **Major tag**: `@v6`

### Changes in v6
- Support and default to Ninja 1.12.1
- Added Windows ARM64 (`winarm64`) and Linux ARM64 (`linux-aarch64`) platform support
- Updated runtime to Node.js 20
- Updated dependencies

### Breaking Changes / Migration Notes
🛑 **REPOSITORY ARCHIVED** - **No further development or maintenance**
> "This action is no longer necessary, as ninja is now included on all default GitHub runner instances. If you do still need this functionality, making a fork of this action into your repository or organization is the recommended path." — [Repository README](https://github.com/seanmiddleditch/gha-setup-ninja)

- **Major version bump**: v5 → v6 (semver-major)
- Default Ninja version changed: 1.11.1 → 1.12.1
- Runtime changed: Node.js 16 → Node.js 20

### Recommendation
**Remove this action** from workflows since Ninja is pre-installed on all GitHub-hosted runners. If a specific Ninja version is needed, consider:
- Using `actions/setup-ninja` (if available)
- Installing manually via package manager in workflow
- Forking the action to your own org

### Official Sources
- Releases: https://github.com/seanmiddleditch/gha-setup-ninja/releases
- Repository: https://github.com/seanmiddleditch/gha-setup-ninja (ARCHIVED)
- Compare v5→v6: https://github.com/seanmiddleditch/gha-setup-ninja/compare/v5...v6

---

## 3. `jurplel/install-qt-action` (currently `@v4`)

### Latest Stable Version
- **Version**: `v4.3.1` (tag: `v4.3.1`)
- **Released**: April 8, 2026
- **Major tag**: `@v4` (points to latest v4.x)

### Version 4.x Breaking Changes (v4.0.0)
- Updated `aqtinstall` to v3.1.* by default ([Upgrade Guide](https://github.com/jurplel/install-qt-action/blob/master/README_upgrade_guide.md))
- SHA256 checksum validation for Qt archives (may cause failures within 24h of new Qt releases)
- Commercial Qt license support added (`use-official: true` with email/password)

### Key Changes in v4.3.x
- **v4.3.1**: Cache key fix for wildcard versions (e.g., `6.*.*` now resolves to actual version for cache key) — [PR #335](https://github.com/jurplel/install-qt-action/pull/335), [Issue #304](https://github.com/jurplel/install-qt-action/issues/304)
- **v4.2.0+**: WASM support requires `host: 'all_os'` and `target: 'wasm'` for Qt 6.7+
- **v4.2.0+**: Commercial Qt support via `use-official: true`
- Default Qt version: `6.8.3` (LTS)
- Updated `@actions/cache` to v4

### Migration Notes for v3 → v4
- `aqtinstall` v3.1+ requires SHA256 checksums (may fail for brand new Qt releases)
- WASM installation syntax changed
- Commercial license requires Qt account credentials
- Cache behavior with wildcards fixed in v4.3.1

### Official Sources
- Releases: https://github.com/jurplel/install-qt-action/releases
- Repository: https://github.com/jurplel/install-qt-action
- Upgrade Guide: https://github.com/jurplel/install-qt-action/blob/master/README_upgrade_guide.md
- Marketplace: https://github.com/marketplace/actions/install-qt

---

## 4. `mozilla-actions/sccache-action` (currently `@v0.0.11`)

### Latest Stable Version
- **Version**: `v0.0.11` (tag: `v0.0.11`)
- **Released**: After v0.0.10 (commit [9e7fa8a](https://github.com/mozilla-actions/sccache-action/commit/9e7fa8a12102821edf02ca5dbea1acd0f89a2696))
- **Note**: Uses 0.0.x versioning (pre-1.0)

### Critical Changes in v0.0.9
- **Forces GitHub Actions Cache Service v2**: Sets `ACTIONS_CACHE_SERVICE_V2=on` ([PR #192](https://github.com/Mozilla-Actions/sccache-action/pull/192))
- Required fix for GitHub Actions cache service brownout ([mozilla/sccache#2351](https://github.com/mozilla/sccache/issues/2351))
- Minimum sccache version: v0.10.0 (v0.11.0+ recommended)

### Changes in v0.0.10 → v0.0.11
- Dependency updates (`@actions/github` 6.0.0 → 6.0.1)
- Preparatory version bump

### Breaking Changes / Migration Notes
- **v0.0.9+ requires GitHub Actions Cache Service v2** — workflows on older runner images may need updates
- sccache versions prior to v0.11.0 "probably will not work"
- Current sccache latest: v0.17.0 (July 2025)
- Action version (0.0.x) ≠ sccache version (0.x.x) — configure `version` input for specific sccache version

### Recommended Configuration
```yaml
- uses: mozilla-actions/sccache-action@v0.0.11
  with:
    version: "v0.17.0"  # Pin to latest sccache
```

### Official Sources
- Releases: https://github.com/Mozilla-Actions/sccache-action/releases
- Repository: https://github.com/mozilla-actions/sccache-action
- sccache releases: https://github.com/mozilla/sccache/releases
- Cache v2 migration: https://github.com/mozilla/sccache/issues/2351

---

## Summary Table

| Action | Current | Latest | Status | Action Required |
|--------|---------|--------|--------|-----------------|
| `ilammy/msvc-dev-cmd` | `@v1` | `v1.13.0` | Active | Monitor for Node.js 24 update (deadline June 2026) |
| `seanmiddleditch/gha-setup-ninja` | `@v5` | `v6` | **ARCHIVED** | **Remove** — Ninja pre-installed on runners |
| `jurplel/install-qt-action` | `@v4` | `v4.3.1` | Active | Update to v4.3.1 for cache wildcard fix |
| `mozilla-actions/sccache-action` | `@v0.0.11` | `v0.0.11` | Active | Pin sccache version via `version` input |

---

## Recommended Workflow Updates

```yaml
# .github/workflows/build.yml - Suggested updates

# 1. msvc-dev-cmd: Keep @v1 (auto-updates to latest v1.x)
#    Monitor for v1.14.0+ with Node.js 24 support
- uses: ilammy/msvc-dev-cmd@v1

# 2. gha-setup-ninja: REMOVE - ninja is pre-installed
# - uses: seanmiddleditch/gha-setup-ninja@v5  # DELETE THIS LINE

# 3. install-qt-action: Update to latest v4 patch
- uses: jurplel/install-qt-action@v4.3.1
  # Or keep @v4 for auto-patch updates

# 4. sccache-action: Pin sccache version explicitly
- uses: mozilla-actions/sccache-action@v0.0.11
  with:
    version: "v0.17.0"
```