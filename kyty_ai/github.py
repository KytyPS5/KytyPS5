from __future__ import annotations

import json
from pathlib import Path

from .utils import run_cmd, trim


class PullRequestInfo:
    def __init__(self, data: dict, patch: str):
        self.data = data
        self.patch = patch

    @property
    def number(self) -> int:
        return int(self.data["number"])

    @property
    def title(self) -> str:
        return self.data.get("title", "")

    @property
    def body(self) -> str:
        return self.data.get("body", "") or ""

    @property
    def base_oid(self) -> str:
        return self.data.get("baseRefOid", "")

    @property
    def head_oid(self) -> str:
        return self.data.get("headRefOid", "")

    @property
    def changed_files(self) -> list[dict]:
        return self.data.get("files", []) or []

    def prompt_summary(self) -> str:
        files = "\n".join(
            f"- {f.get('path')} (+{f.get('additions', 0)}/-{f.get('deletions', 0)})"
            for f in self.changed_files
        )
        return (
            f"PR #{self.number}: {self.title}\n"
            f"URL: {self.data.get('url', '')}\n"
            f"Base: {self.data.get('baseRefName', '')} {self.base_oid}\n"
            f"Head: {self.data.get('headRefName', '')} {self.head_oid}\n\n"
            f"Description:\n{self.body}\n\n"
            f"Changed files:\n{files}\n"
        )


def fetch_pr(root: Path, repo: str, number: int) -> PullRequestInfo:
    meta = run_cmd(
        [
            "gh", "pr", "view", str(number),
            "--repo", repo,
            "--json",
            "number,title,body,url,baseRefName,baseRefOid,headRefName,headRefOid,files,commits"
        ],
        cwd=root,
        timeout=180,
        check=True,
    ).stdout
    data = json.loads(meta)

    patch = run_cmd(
        ["gh", "pr", "diff", str(number), "--repo", repo, "--patch", "--color", "never"],
        cwd=root,
        timeout=300,
        check=True,
    ).stdout

    return PullRequestInfo(data, trim(patch, 500_000))
