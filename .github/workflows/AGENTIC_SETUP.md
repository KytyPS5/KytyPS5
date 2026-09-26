# GitHub Agentic Workflow setup

`copilot-ci-fix.md` is a GitHub Agentic Workflow source file. Current GitHub Agentic Workflows are authored as Markdown with YAML frontmatter and then compiled into a `.lock.yml` workflow before they are committed to the default branch.

Install the GitHub CLI extension:

```bash
gh extension install github/gh-aw
```

Initialize the repository for agentic workflow authoring:

```bash
gh aw init
```

Compile this workflow:

```bash
gh aw compile .github/workflows/copilot-ci-fix.md
```

Commit both:

```text
.github/workflows/copilot-ci-fix.md
.github/workflows/copilot-ci-fix.lock.yml
```

The workflow uses the Copilot engine through `copilot-requests: write`. That requires Copilot access for the repository/account or organization billing configuration. It intentionally only has read permissions for repository contents and uses the safe `create-pull-request` output for proposed code changes.

To test the complete system:

1. Run `KytyPS5 Integration Build` manually.
2. Select `none` for the upstream source and leave patch/file inputs empty.
3. Keep `create_release` disabled for the first test.
4. Confirm the generated integration branch builds through `Build and Release KytyPS5`.
5. Trigger `Manual KytyPS5 Release` only after the build path is known to work.
6. To test the Copilot workflow, use a controlled CI failure in a non-production branch and verify that it proposes a draft PR rather than modifying the default branch directly.
