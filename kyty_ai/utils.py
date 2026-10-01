from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path
from typing import Any


def run_cmd(
    args: list[str],
    cwd: Path,
    timeout: int,
    *,
    check: bool = False,
) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        args,
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
        encoding="utf-8",
        errors="replace",
    )
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"Command failed ({proc.returncode}): {' '.join(args)}\n{proc.stdout}"
        )
    return proc


def git(cwd: Path, args: list[str], timeout: int = 120) -> str:
    return run_cmd(["git", *args], cwd, timeout, check=True).stdout.strip()


def repo_root(start: Path) -> Path:
    return Path(git(start, ["rev-parse", "--show-toplevel"]))


def current_branch(root: Path) -> str:
    return git(root, ["branch", "--show-current"])


def current_commit(root: Path) -> str:
    return git(root, ["rev-parse", "HEAD"])


def safe_name(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value)
    return value.strip("-")[:90] or "unnamed"


def load_config(root: Path) -> dict[str, Any]:
    path = root / "kyty-ai.json"
    if not path.exists():
        example = root / "kyty-ai.example.json"
        if example.exists():
            return json.loads(example.read_text(encoding="utf-8"))
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")


def trim(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    half = max(1, (limit - 200) // 2)
    return (
        text[:half]
        + "\n\n...[TRUNCATED BY KYTY-AI]...\n\n"
        + text[-half:]
    )


def is_probably_text(path: Path) -> bool:
    suffix = path.suffix.lower()
    if suffix in {
        ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".ico",
        ".zip", ".7z", ".rar", ".gz", ".xz", ".bz2", ".pdf",
        ".exe", ".dll", ".so", ".dylib", ".bin", ".a", ".lib",
        ".pdb", ".obj", ".o", ".woff", ".woff2", ".ttf",
    }:
        return False
    return True
