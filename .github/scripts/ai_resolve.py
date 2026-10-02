#!/usr/bin/env python3
"""AI helper for port-upstream-pr.yml.

  ai_resolve.py resolve   resolve leftover conflicts (<<<<<<< markers) and *.rej files
  ai_resolve.py repair    adapt the ported PR so the verify command passes on the fork
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

# GitHub Models (free tier): small request limits, so keep prompts compact.
API = "https://models.github.ai/inference/chat/completions"
MODEL = os.environ.get("AI_MODEL") or "openai/gpt-4.1"
KEY = os.environ.get("AI_TOKEN", "")
MAX_FILE = int(os.environ.get("AI_MAX_FILE_CHARS", 10_000))
MAX_CTX = 3_000
MAX_OUT = 4_000
REPORT = os.path.join(os.environ.get("RUNNER_TEMP", "/tmp"), "ai_report.json")

MARKER = re.compile(r"^(<{7}|>{7})( .*)?$", re.M)
RESOLVED = re.compile(r"<resolved_file>\n?(.*)</resolved_file>", re.S)
NOTE = re.compile(r"<note>(.*?)</note>", re.S)
FILE_BLOCK = re.compile(r'<file path="([^"]+)">\n?(.*?)\n?</file>', re.S)

RESOLVE_SYSTEM = """You port an upstream pull request onto a fork whose code has diverged.
Resolve the conflicts so the result keeps the PR's intent AND the fork's own changes.
Never drop fork-specific behaviour unless the PR clearly supersedes it. No unrelated edits.
In conflict markers, the first side (<<<<<<<) is the FORK, the second (>>>>>>>) is the PR.
Reply with exactly:
<resolved_file>
...the entire final file, no conflict markers...
</resolved_file>
<note>one short sentence describing how you resolved it</note>"""

REPAIR_SYSTEM = """You adapt a pull request that was ported onto a diverged fork so the fork's verify command passes.
Fix only what the failure output points to, keeping the PR's intent and the fork's behaviour.
You may only modify files listed in the prompt. Reply with one block per modified file:
<file path="relative/path">
...entire new file content...
</file>
and finish with <note>one short sentence</note>. If you cannot help, reply with <note> only."""


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True, errors="replace").stdout


def clip(s, n):
    return s if len(s) <= n else s[:n] + "\n... [truncated]"


def ask(system, user):
    body = json.dumps({
        "model": MODEL,
        "max_tokens": MAX_OUT,
        "temperature": 0.1,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
    }).encode()
    for attempt in range(4):
        req = urllib.request.Request(API, body, {
            "Authorization": f"Bearer {KEY}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/json",
        })
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                data = json.load(r)
            return data["choices"][0]["message"]["content"]
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")[:300]
            print(f"::warning::API error {e.code} (attempt {attempt + 1}): {detail}")
            if e.code == 429:  # free-tier rate limit: honour Retry-After
                time.sleep(min(int(e.headers.get("Retry-After", 30)), 90))
            elif e.code in (400, 401, 403, 413):
                return None  # won't succeed on retry
            else:
                time.sleep(5 * (attempt + 1))
        except (urllib.error.URLError, TimeoutError, KeyError, ValueError) as e:
            print(f"::warning::API call failed (attempt {attempt + 1}): {e}")
            time.sleep(5 * (attempt + 1))
    return None


def load_report():
    try:
        with open(REPORT) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"resolved": [], "unresolved": [], "repairs": []}


def save_report(rep):
    with open(REPORT, "w") as f:
        json.dump(rep, f, indent=2)


def note_of(out):
    m = NOTE.search(out or "")
    return (m.group(1).strip().replace("\n", " ") if m else "no note")[:300]


def handle(rep, path, src, is_reject):
    def fail(reason):
        print(f"::warning::{path}: {reason}")
        rep["unresolved"].append({"path": path, "reason": reason})

    try:
        cur = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        rej = open(src, encoding="utf-8").read() if is_reject else ""
    except (UnicodeDecodeError, OSError):
        return fail("binary or unreadable file")
    if not is_reject and not MARKER.search(cur):
        return fail("no text conflict markers (modify/delete or binary conflict)")
    if len(cur) + len(rej) > MAX_FILE:
        return fail("file too large for AI resolution")

    pr_delta = clip(sh("git", "diff", os.environ["PR_BASE"], os.environ["PR_REF"], "--", path), MAX_CTX)
    fork_delta = clip(sh("git", "diff", f"{os.environ['FORK_BASE']}..HEAD", "--", path), MAX_CTX)
    try:
        pr_desc = clip(open(os.environ["PR_BODY_FILE"], encoding="utf-8").read(), 1000)
    except OSError:
        pr_desc = ""

    if is_reject:
        subject = (f"## Current file (PR hunks that failed to apply are listed below)\n```\n{cur}\n```\n"
                   f"## Rejected hunks\n```diff\n{rej}\n```")
    else:
        subject = f"## File with conflict markers\n```\n{cur}\n```"

    prompt = (f"PR title: {os.environ['PR_TITLE']}\nPR description:\n{pr_desc}\n\nFile: {path}\n\n"
              f"## What the upstream PR changes in this file\n```diff\n{pr_delta}\n```\n"
              f"## How the fork already differs from upstream in this file\n```diff\n{fork_delta}\n```\n"
              f"{subject}")

    out = ask(RESOLVE_SYSTEM, prompt)
    m = RESOLVED.search(out or "")
    if not m:
        return fail("AI returned no usable output")
    new = m.group(1).strip("\n")
    if MARKER.search(new):
        return fail("AI output still contains conflict markers")
    new += "\n" if (cur.endswith("\n") or not cur) else ""

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(new)
    sh("git", "add", "--", path)
    if is_reject:
        os.remove(src)
    rep["resolved"].append({"path": path, "note": note_of(out)})
    print(f"resolved {path}")


def resolve(rep):
    conflicts = [p for p in sh("git", "diff", "--name-only", "--diff-filter=U").splitlines() if p]
    rejects = [p for p in sh("git", "ls-files", "--others", "--exclude-standard").splitlines()
               if p.endswith(".rej")]
    for p in conflicts:
        handle(rep, p, p, False)
    for r in rejects:
        handle(rep, r[:-4], r, True)
    save_report(rep)
    return 0


def repair(rep):
    log = open(os.environ["VERIFY_LOG"], errors="replace").read()[-4000:]
    base = os.environ["TARGET_REF"]
    files = [p for p in sh("git", "diff", "--name-only", base, "HEAD").splitlines() if os.path.isfile(p)]

    blocks, budget = [], MAX_FILE
    for p in files:
        try:
            t = open(p, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        if len(t) > budget:
            continue
        budget -= len(t)
        blocks.append(f'<file path="{p}">\n{t}\n</file>')

    prompt = (f"Verify command output (tail):\n```\n{log}\n```\n\n"
              f"Ported diff vs fork base:\n```diff\n{clip(sh('git', 'diff', base, 'HEAD'), 6000)}\n```\n\n"
              f"Modifiable files:\n" + "\n".join(blocks))
    out = ask(REPAIR_SYSTEM, prompt)

    changed = []
    for m in FILE_BLOCK.finditer(out or ""):
        p, text = m.group(1), m.group(2)
        if p not in files or MARKER.search(text):
            continue
        with open(p, "w", encoding="utf-8") as f:
            f.write(text.rstrip("\n") + "\n")
        sh("git", "add", "--", p)
        changed.append(p)

    rep.setdefault("repairs", []).append({"files": changed, "note": note_of(out)})
    save_report(rep)
    return 0 if changed else 1


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "resolve"
    if not KEY:
        print("::warning::AI_TOKEN not set - skipping AI step")
        return 0 if mode == "resolve" else 1
    rep = load_report()
    return repair(rep) if mode == "repair" else resolve(rep)


if __name__ == "__main__":
    sys.exit(main())
