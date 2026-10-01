from __future__ import annotations

from pathlib import Path
from typing import Any

from .utils import run_cmd, trim


def run_step(root: Path, command: list[str], timeout: int, label: str) -> dict[str, Any]:
    if not command:
        return {
            "label": label,
            "status": "SKIPPED",
            "returncode": None,
            "output": "No command configured.",
        }

    proc = run_cmd(command, cwd=root, timeout=timeout, check=False)
    return {
        "label": label,
        "status": "PASS" if proc.returncode == 0 else "FAIL",
        "returncode": proc.returncode,
        "output": trim(proc.stdout, 120_000),
        "command": command,
    }


def verify(root: Path, cfg: dict) -> list[dict[str, Any]]:
    build_cfg = cfg.get("build", {})
    runtime_cfg = cfg.get("runtime", {})
    cwd = root / build_cfg.get("working_directory", ".")
    timeout = int(build_cfg.get("timeout_seconds", 1800))

    steps: list[dict[str, Any]] = []

    for label, key in [
        ("configure", "configure"),
        ("build", "build"),
        ("test", "test"),
    ]:
        command = list(build_cfg.get(key, []))
        if not command:
            continue
        result = run_step(cwd, command, timeout, label)
        steps.append(result)
        if result["status"] == "FAIL":
            return steps

    runtime_command = list(runtime_cfg.get("command", []))
    if runtime_command:
        steps.append(
            run_step(
                cwd,
                runtime_command,
                int(runtime_cfg.get("timeout_seconds", 300)),
                "runtime",
            )
        )

    return steps


def failure_text(steps: list[dict[str, Any]]) -> str:
    failures = [s for s in steps if s["status"] == "FAIL"]
    if not failures:
        return ""
    return "\n\n".join(
        f"===== {s['label'].upper()} FAILURE =====\n{s['output']}"
        for s in failures
    )
