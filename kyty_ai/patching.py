from __future__ import annotations

import re
from pathlib import Path

from .ollama import generate
from .prompts import ANALYSIS_SYSTEM, PATCH_SYSTEM, REPAIR_SYSTEM
from .utils import git, run_cmd, trim


def extract_diff(text: str) -> str:
    fenced = re.findall(r"```(?:diff|patch)?\s*(diff --git[\s\S]*?)```", text, re.I)
    if fenced:
        return fenced[-1].strip()

    marker = text.find("diff --git")
    if marker >= 0:
        return text[marker:].strip()

    return ""


def analyze_pr(
    *,
    ollama_cfg: dict,
    pr_summary: str,
    patch: str,
    current_context: str,
    history: str,
) -> str:
    prompt = (
        ANALYSIS_SYSTEM
        + "\n\n===== PR =====\n"
        + pr_summary
        + "\n===== UPSTREAM PATCH =====\n"
        + trim(patch, 320_000)
        + "\n===== CURRENT SOURCE =====\n"
        + trim(current_context, 220_000)
        + "\n===== RECENT HISTORY =====\n"
        + trim(history, 15_000)
    )
    return generate(
        url=ollama_cfg["url"],
        model=ollama_cfg["model"],
        prompt=prompt,
        timeout=int(ollama_cfg.get("timeout_seconds", 900)),
        temperature=float(ollama_cfg.get("temperature", 0.1)),
    )


def make_patch(
    *,
    ollama_cfg: dict,
    pr_summary: str,
    patch: str,
    current_context: str,
    history: str,
    analysis: str,
    failure: str = "",
) -> str:
    system = REPAIR_SYSTEM if failure else PATCH_SYSTEM
    prompt = (
        system
        + "\n\n===== PR =====\n"
        + pr_summary
        + "\n===== UPSTREAM PATCH =====\n"
        + trim(patch, 320_000)
        + "\n===== CURRENT SOURCE =====\n"
        + trim(current_context, 220_000)
        + "\n===== RECENT HISTORY =====\n"
        + trim(history, 15_000)
        + "\n===== ANALYSIS PLAN =====\n"
        + trim(analysis, 30_000)
    )
    if failure:
        prompt += "\n===== BUILD/TEST FAILURE =====\n" + trim(failure, 80_000)

    answer = generate(
        url=ollama_cfg["url"],
        model=ollama_cfg["model"],
        prompt=prompt,
        timeout=int(ollama_cfg.get("timeout_seconds", 900)),
        temperature=float(ollama_cfg.get("temperature", 0.1)),
    )
    return extract_diff(answer)


def apply_patch_checked(worktree: Path, patch: str, *, timeout: int = 120) -> tuple[bool, str]:
    patch_file = worktree / ".ai-last.patch"
    patch_file.write_text(patch, encoding="utf-8")

    check = run_cmd(
        ["git", "apply", "--check", "--recount", str(patch_file)],
        cwd=worktree,
        timeout=timeout,
        check=False,
    )
    if check.returncode != 0:
        return False, check.stdout

    applied = run_cmd(
        ["git", "apply", "--recount", str(patch_file)],
        cwd=worktree,
        timeout=timeout,
        check=False,
    )
    if applied.returncode != 0:
        return False, applied.stdout

    return True, "patch applied successfully"


def uncommitted_diff(worktree: Path) -> str:
    return git(worktree, ["diff", "--no-ext-diff"], timeout=120)
