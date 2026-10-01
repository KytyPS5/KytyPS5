from __future__ import annotations

import argparse
import json
import sys

from .agent import analyze, get_root, integrate_stack, verify_current


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="kyty-ai",
        description="Local Ollama-assisted semantic PR integration for KytyPS5.",
    )
    sub = p.add_subparsers(dest="command", required=True)

    a = sub.add_parser("analyze-pr", help="Analyze a GitHub PR against the current checkout.")
    a.add_argument("number", type=int)
    a.add_argument("--path", default=".")

    i = sub.add_parser(
        "integrate",
        help="Integrate one or more PRs cumulatively in one isolated worktree."
    )
    i.add_argument("numbers", nargs="+", type=int)
    i.add_argument("--path", default=".")

    v = sub.add_parser("verify", help="Run configured build/test/runtime commands in the current checkout.")
    v.add_argument("--path", default=".")

    return p


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)

    try:
        root = get_root(getattr(args, "path", "."))
        print(f"Repository: {root}")

        if args.command == "analyze-pr":
            result = analyze(root, args.number)
            print(result["analysis"])
            print(f"\nSaved: {result['analysis_file']}")
            return 0

        if args.command == "verify":
            results = verify_current(root)
            for step in results:
                print(f"\n[{step['status']}] {step['label']}")
                print(step["output"][-12000:])
            return 0 if all(s["status"] in {"PASS", "SKIPPED"} for s in results) else 1

        if args.command == "integrate":
            result = integrate_stack(root, args.numbers)
            print("\n" + json.dumps(result, indent=2))
            if not result.get("success"):
                print(
                    "\nThe stack was not fully verified. "
                    "The isolated worktree and report were preserved."
                )
            return 0 if result.get("success") else 1

        return 2

    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
