from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import Any

from .github import fetch_pr
from .patching import analyze_pr, apply_patch_checked, make_patch, uncommitted_diff
from .source_context import collect_changed_source, collect_recent_history
from .utils import current_branch, current_commit, git, load_config, repo_root, write_json
from .verify import failure_text, verify
from .worktree import assert_safe_base, create


def _pr_paths(info) -> list[str]:
    return [f.get("path", "") for f in info.changed_files if f.get("path")]


def _current_diff_paths(worktree: Path) -> list[str]:
    raw = git(worktree, ["diff", "--name-only"], timeout=120)
    return [line.strip() for line in raw.splitlines() if line.strip()]


def _context_for(worktree: Path, paths: list[str], cfg: dict[str, Any]) -> tuple[str, str, list[str]]:
    integ = cfg.get("integration", {})
    merged_paths = list(dict.fromkeys(paths + _current_diff_paths(worktree)))
    context = collect_changed_source(
        worktree,
        merged_paths,
        max_file_bytes=int(integ.get("max_file_bytes", 180000)),
        max_context_chars=int(integ.get("max_context_chars", 260000)),
    )
    history = collect_recent_history(worktree, merged_paths)
    return context, history, merged_paths


def analyze(root: Path, number: int) -> dict[str, Any]:
    cfg = load_config(root)
    repo = cfg.get("repository", {}).get("repo", "KytyPS5/KytyPS5")
    info = fetch_pr(root, repo, number)
    context, history, _ = _context_for(root, _pr_paths(info), cfg)

    analysis = analyze_pr(
        ollama_cfg=cfg["ollama"],
        pr_summary=info.prompt_summary(),
        patch=info.patch,
        current_context=context,
        history=history,
    )

    ai_dir = root / ".ai" / "analyses"
    ai_dir.mkdir(parents=True, exist_ok=True)
    path = ai_dir / f"pr-{number}.md"
    path.write_text(analysis, encoding="utf-8")

    return {
        "pr": number,
        "title": info.title,
        "base_commit": current_commit(root),
        "branch": current_branch(root),
        "analysis_file": str(path),
        "analysis": analysis,
    }


def _integrate_pr_into_worktree(
    *,
    root: Path,
    worktree: Path,
    cfg: dict[str, Any],
    number: int,
    analysis: str | None = None,
) -> dict[str, Any]:
    repo = cfg.get("repository", {}).get("repo", "KytyPS5/KytyPS5")
    info = fetch_pr(root, repo, number)
    pr_paths = _pr_paths(info)
    context, history, context_paths = _context_for(worktree, pr_paths, cfg)

    if not analysis:
        analysis = analyze_pr(
            ollama_cfg=cfg["ollama"],
            pr_summary=info.prompt_summary(),
            patch=info.patch,
            current_context=context,
            history=history,
        )

    repairs = 0
    verification_runs: list[list[dict[str, Any]]] = []

    patch = make_patch(
        ollama_cfg=cfg["ollama"],
        pr_summary=info.prompt_summary(),
        patch=info.patch,
        current_context=context,
        history=history,
        analysis=analysis,
    )
    if not patch:
        return {
            "success": False,
            "pr": number,
            "title": info.title,
            "error": "The model did not return a usable unified diff.",
            "repairs": repairs,
        }

    ok, message = apply_patch_checked(worktree, patch)
    max_repairs = int(cfg.get("integration", {}).get("max_repairs", 3))

    while not ok and repairs < max_repairs:
        repairs += 1
        context, history, context_paths = _context_for(
            worktree, pr_paths + ["CMakeLists.txt", "CMakePresets.json"], cfg
        )
        patch = make_patch(
            ollama_cfg=cfg["ollama"],
            pr_summary=info.prompt_summary(),
            patch=info.patch,
            current_context=context,
            history=history,
            analysis=analysis,
            failure="git apply --check failure:\n" + message,
        )
        if not patch:
            break
        ok, message = apply_patch_checked(worktree, patch)

    if not ok:
        return {
            "success": False,
            "pr": number,
            "title": info.title,
            "error": f"Patch could not be applied after {repairs} repair attempt(s):\n{message}",
            "repairs": repairs,
            "analysis": analysis,
            "diff": uncommitted_diff(worktree),
        }

    while True:
        steps = verify(worktree, cfg)
        verification_runs.append(steps)
        failure = failure_text(steps)

        if not failure:
            break

        if repairs >= max_repairs:
            break

        repairs += 1
        current_diff = uncommitted_diff(worktree)
        fresh_context, fresh_history, _ = _context_for(
            worktree,
            context_paths + ["CMakeLists.txt", "CMakePresets.json"],
            cfg,
        )
        patch = make_patch(
            ollama_cfg=cfg["ollama"],
            pr_summary=info.prompt_summary(),
            patch=info.patch,
            current_context=fresh_context,
            history=fresh_history,
            analysis=analysis,
            failure=failure + "\n\nCURRENT UNCOMMITTED DIFF:\n" + current_diff,
        )
        if not patch:
            break

        ok, message = apply_patch_checked(worktree, patch)
        if not ok:
            # The next repair iteration gets the apply failure as its failure
            # signal and may regenerate the patch.
            continue

    success = bool(verification_runs) and all(
        step["status"] in {"PASS", "SKIPPED"}
        for steps in verification_runs
        for step in steps
    )

    return {
        "success": success,
        "pr": number,
        "title": info.title,
        "repairs": repairs,
        "analysis": analysis,
        "verification": verification_runs,
        "diff": uncommitted_diff(worktree),
    }


def _commit_integration(worktree: Path, number: int) -> str | None:
    status = git(worktree, ["status", "--porcelain"])
    # `.ai-last.patch` is temporary and must never enter an integration commit.
    temp_patch = worktree / ".ai-last.patch"
    if temp_patch.exists():
        temp_patch.unlink()

    if not status.strip():
        return None

    git(worktree, ["add", "-A"], timeout=180)
    git(
        worktree,
        ["commit", "-m", f"ai: semantically integrate upstream PR #{number}"],
        timeout=300,
    )
    return git(worktree, ["rev-parse", "HEAD"])


def integrate_stack(root: Path, numbers: list[int]) -> dict[str, Any]:
    cfg = load_config(root)
    integ = cfg.get("integration", {})
    assert_safe_base(
        root,
        list(integ.get("protected_branches", ["main", "master"])),
    )

    base_commit = current_commit(root)
    base_branch = current_branch(root)
    worktree_root = root / integ.get("worktree_root", ".ai/worktrees")
    if not worktree_root.is_absolute():
        worktree_root = root / worktree_root

    label = "stack-" + "-".join(map(str, numbers))
    worktree, branch = create(root, base_branch, worktree_root, label)

    reports: list[dict[str, Any]] = []
    success = True

    try:
        for number in numbers:
            print(f"\n=== PR #{number} on cumulative stack ===")
            result = _integrate_pr_into_worktree(
                root=root,
                worktree=worktree,
                cfg=cfg,
                number=number,
            )

            if result.get("success"):
                result["commit"] = _commit_integration(worktree, number)
                print(f"PR #{number}: VERIFIED")
                if result.get("commit"):
                    print(f"Committed: {result['commit']}")
            else:
                print(f"PR #{number}: FAILED")
                success = False

            reports.append(result)

            if not result.get("success"):
                # Preserve the worktree exactly where the stack failed.
                break

        report_dir = root / ".ai" / "stack-reports"
        report_dir.mkdir(parents=True, exist_ok=True)
        timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        report_file = report_dir / f"stack-{timestamp}.json"

        payload = {
            "success": success,
            "prs": numbers,
            "base_branch": base_branch,
            "base_commit": base_commit,
            "temporary_branch": branch,
            "worktree": str(worktree),
            "reports": reports,
            "final_commit": git(worktree, ["rev-parse", "HEAD"]),
            "final_diff": uncommitted_diff(worktree),
        }
        write_json(report_file, payload)

        return {
            "success": success,
            "temporary_branch": branch,
            "worktree": str(worktree),
            "report": str(report_file),
            "final_commit": payload["final_commit"],
            "completed": [
                r["pr"] for r in reports if r.get("success")
            ],
        }

    except Exception as exc:
        report_dir = root / ".ai" / "stack-reports"
        report_dir.mkdir(parents=True, exist_ok=True)
        timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        report_file = report_dir / f"stack-{timestamp}-FAILED.json"
        write_json(
            report_file,
            {
                "success": False,
                "prs": numbers,
                "base_branch": base_branch,
                "base_commit": base_commit,
                "temporary_branch": branch,
                "worktree": str(worktree),
                "reports": reports,
                "error": str(exc),
                "final_commit": git(worktree, ["rev-parse", "HEAD"]),
                "final_diff": uncommitted_diff(worktree),
            },
        )
        return {
            "success": False,
            "temporary_branch": branch,
            "worktree": str(worktree),
            "report": str(report_file),
            "error": str(exc),
        }
