KytyPS5 integration patches

Put unified Git patches (*.patch) in this directory when you want a repeatable local patch set.

Example:

patches/
  0001-my-change.patch
  0002-follow-up.patch

Apply them locally with:

./scripts/apply-patches.sh

The KytyPS5 Integration Build workflow also accepts a patch directly through the patch_text workflow input. From the GitHub CLI, a local patch can be passed with:

gh workflow run integration-build.yml \
  -f upstream_source=none \
  -F patch_text=@0001-my-change.patch
