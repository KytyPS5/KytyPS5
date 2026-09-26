#!/usr/bin/env bash
set -euo pipefail

UPSTREAM_URL="${1:-https://github.com/KytyPS5/KytyPS5.git}"
UPSTREAM_BRANCH="${2:-main}"

if git remote get-url upstream >/dev/null 2>&1; then
  git remote set-url upstream "$UPSTREAM_URL"
else
  git remote add upstream "$UPSTREAM_URL"
fi

git fetch --prune --tags upstream "$UPSTREAM_BRANCH"

echo "Fork HEAD:    $(git rev-parse HEAD)"
echo "Upstream HEAD:$(git rev-parse "upstream/$UPSTREAM_BRANCH")"

echo

git log --oneline --decorate --no-renames \
  "HEAD..upstream/$UPSTREAM_BRANCH" | head -n 50
