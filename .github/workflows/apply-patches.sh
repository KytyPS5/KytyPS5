#!/usr/bin/env bash
set -euo pipefail

PATCH_DIR="${1:-patches}"

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
shopt -u nullglob

if [ "${#patches[@]}" -eq 0 ]; then
  echo "No .patch files found in $PATCH_DIR."
  exit 0
fi

for patch in "${patches[@]}"; do
  echo "Applying $patch"
  git apply --3way --index "$patch"
done

git diff --cached --check

echo "All patches applied successfully."
