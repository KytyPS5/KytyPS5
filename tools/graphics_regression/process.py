"""Run and reliably terminate one isolated emulator process tree."""

from __future__ import annotations

import os
import signal
import subprocess
from pathlib import Path

TIMEOUT_EXIT_CODE = 124
_CREATE_SUSPENDED = 0x00000004


def _terminate_tree(process: subprocess.Popen[bytes], windows_job: int | None) -> None:
    if os.name == "nt":
        if windows_job is not None:
            from . import windows_job as jobs
            jobs.terminate(windows_job, TIMEOUT_EXIT_CODE)
        elif process.poll() is None:
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run_process(command: list[str], cwd: Path, log: Path, timeout_seconds: float) -> tuple[int, bool]:
    creationflags = 0
    if os.name == "nt":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP | _CREATE_SUSPENDED
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                   creationflags=creationflags, start_new_session=os.name != "nt")
        job: int | None = None
        try:
            if os.name == "nt":
                from . import windows_job as jobs
                handle = int(process._handle)  # type: ignore[attr-defined]
                job = jobs.attach(handle)
                if job is None:
                    raise OSError("could not assign emulator to a kill-on-close Windows Job Object")
                if not jobs.resume(handle):
                    raise OSError("could not resume emulator after Windows Job Object assignment")
            try:
                exit_code = process.wait(timeout=timeout_seconds)
                if os.name != "nt":
                    _terminate_tree(process, job)
                return exit_code, False
            except subprocess.TimeoutExpired:
                _terminate_tree(process, job)
                return TIMEOUT_EXIT_CODE, True
        except BaseException:
            _terminate_tree(process, job)
            raise
        finally:
            if job is not None:
                from . import windows_job as jobs
                jobs.close(job)
