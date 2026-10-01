from __future__ import annotations

from pathlib import Path

from .utils import is_probably_text, trim


def collect_changed_source(
    root: Path,
    paths: list[str],
    *,
    max_file_bytes: int,
    max_context_chars: int,
) -> str:
    chunks: list[str] = []
    total = 0

    for rel in paths:
        path = root / rel
        if not path.exists() or not path.is_file() or not is_probably_text(path):
            continue

        try:
            size = path.stat().st_size
            if size > max_file_bytes:
                text = (
                    f"// FILE OMITTED: {rel}\n"
                    f"// Size {size} bytes exceeds max_file_bytes={max_file_bytes}\n"
                )
            else:
                text = path.read_text(encoding="utf-8", errors="replace")
                text = trim(text, max_file_bytes)
        except OSError as exc:
            text = f"// Could not read {rel}: {exc}\n"

        chunk = f"\n===== CURRENT FILE: {rel} =====\n{text}\n"
        if total + len(chunk) > max_context_chars:
            chunks.append(
                "\n===== CONTEXT LIMIT REACHED =====\n"
                "// Remaining files were omitted.\n"
            )
            break

        chunks.append(chunk)
        total += len(chunk)

    return "".join(chunks)


def collect_recent_history(root: Path, paths: list[str]) -> str:
    if not paths:
        return ""
    import subprocess

    result = subprocess.run(
        ["git", "log", "-n", "12", "--oneline", "--", *paths],
        cwd=str(root),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        encoding="utf-8",
        errors="replace",
    )
    return result.stdout[:20000]
