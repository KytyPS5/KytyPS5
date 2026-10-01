from __future__ import annotations

from pathlib import Path

from .utils import current_branch, current_commit, git, safe_name


def create(root: Path, base_branch: str, worktree_root: Path, label: str) -> tuple[Path, str]:
    base = current_commit(root)
    worktree_root.mkdir(parents=True, exist_ok=True)
    path = worktree_root / safe_name(label)
    branch = safe_name(f"ai/{label}-{base[:10]}")

    if path.exists():
        raise RuntimeError(f"Worktree path already exists: {path}")

    git(root, ["worktree", "add", "-b", branch, str(path), base], timeout=180)
    return path, branch


def remove(root: Path, path: Path, *, force: bool = True) -> None:
    args = ["worktree", "remove"]
    if force:
        args.append("--force")
    args.append(str(path))
    git(root, args, timeout=180)


def assert_safe_base(root: Path, protected: list[str]) -> None:
    branch = current_branch(root)
    if branch in protected:
        raise RuntimeError(
            f"Refusing integration from protected branch '{branch}'. "
            f"Create/use a working branch first."
        )
