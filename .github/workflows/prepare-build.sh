#!/usr/bin/env bash
set -euo pipefail

OUTPUT="${1:-build-source.json}"
REPOSITORY="$(git remote get-url origin 2>/dev/null || true)"
UPSTREAM="$(git remote get-url upstream 2>/dev/null || true)"
COMMIT="$(git rev-parse HEAD)"
BRANCH="$(git symbolic-ref --quiet --short HEAD || echo detached)"
DIRTY="false"
if ! git diff --quiet || ! git diff --cached --quiet; then
  DIRTY="true"
fi

python3 - "$OUTPUT" "$REPOSITORY" "$UPSTREAM" "$COMMIT" "$BRANCH" "$DIRTY" <<'PY'
import json
import sys

output, repository, upstream, commit, branch, dirty = sys.argv
payload = {
    "repository": repository,
    "upstream": upstream,
    "commit": commit,
    "branch": branch,
    "dirty": dirty == "true",
}
with open(output, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2)
    f.write("\n")
print(json.dumps(payload, indent=2))
PY
