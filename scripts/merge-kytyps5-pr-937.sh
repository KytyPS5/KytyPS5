#!/bin/sh
set -eu

upstream_url="https://github.com/KytyPS5/KytyPS5.git"
pr_number=937
fetch_ref="refs/kytyps5/pull/$pr_number"

usage() {
	printf 'Usage: %s [--dry-run]\n' "$0" >&2
}

dry_run=false
case "${1:-}" in
	"")
		;;
	--dry-run)
		dry_run=true
		;;
	*)
		usage
		exit 2
		;;
esac
if [ "$#" -gt 1 ]; then
	usage
	exit 2
fi

if ! repo_root=$(git rev-parse --show-toplevel 2>/dev/null); then
	printf 'Error: run this script from inside a Git repository.\n' >&2
	exit 1
fi
cd "$repo_root"

if ! branch=$(git symbolic-ref --quiet --short HEAD); then
	printf 'Error: checkout a branch before merging PR #%s.\n' "$pr_number" >&2
	exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
	printf 'Error: working tree must be clean before merging PR #%s.\n' "$pr_number" >&2
	exit 1
fi

for state in MERGE_HEAD CHERRY_PICK_HEAD REVERT_HEAD; do
	if git rev-parse --quiet --verify "$state" >/dev/null 2>&1; then
		printf 'Error: a Git operation is already in progress (%s).\n' "$state" >&2
		exit 1
	fi
done

for state_dir in rebase-apply rebase-merge; do
	state_path=$(git rev-parse --git-path "$state_dir")
	if [ -d "$state_path" ]; then
		printf 'Error: a Git rebase is already in progress (%s).\n' "$state_dir" >&2
		exit 1
	fi
done

printf 'Fetching KytyPS5 PR #%s into %s...\n' "$pr_number" "$fetch_ref"
git fetch --no-tags "$upstream_url" \
	"+refs/pull/$pr_number/head:$fetch_ref"

if ! git merge-base HEAD "$fetch_ref" >/dev/null 2>&1; then
	printf 'Error: PR #%s has no common history with the current branch; refusing an unrelated merge.\n' "$pr_number" >&2
	exit 1
fi

if git merge-base --is-ancestor "$fetch_ref" HEAD; then
	printf 'PR #%s is already fully included in branch %s.\n' "$pr_number" "$branch"
	exit 0
fi

incoming=$(git rev-list --count "HEAD..$fetch_ref")
head_sha=$(git rev-parse --short "$fetch_ref")
printf 'PR #%s head %s has %s commits not yet in branch %s.\n' \
	"$pr_number" "$head_sha" "$incoming" "$branch"

printf 'Checking whether the merge can be applied cleanly...\n'
if merge_preview=$(git merge-tree --write-tree HEAD "$fetch_ref" 2>&1); then
	:
else
	merge_tree_status=$?
	if [ "$merge_tree_status" -eq 1 ]; then
		printf 'Error: PR #%s conflicts with branch %s; no merge was started.\n' \
			"$pr_number" "$branch" >&2
		printf '%s\n' "$merge_preview" | grep '^CONFLICT ' >&2 || true
		exit 1
	fi
	printf 'Error: could not preflight the merge (git merge-tree exited %s):\n%s\n' \
		"$merge_tree_status" "$merge_preview" >&2
	exit "$merge_tree_status"
fi

if [ "$dry_run" = true ]; then
	printf 'Dry run: no merge was performed.\n'
	exit 0
fi

printf 'Merging PR #%s with a merge commit...\n' "$pr_number"
if git merge --no-ff --no-edit "$fetch_ref"; then
	printf 'PR #%s merged into branch %s.\n' "$pr_number" "$branch"
else
	printf 'Error: merge did not complete. Resolve any conflicts, then commit the merge.\n' >&2
	exit 1
fi
