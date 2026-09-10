#!/usr/bin/env bash
# Launches the Kyty Launcher (Tauri UI).
#
# Default: builds the release binary if needed and runs it standalone, the
# same binary the .desktop entry launches (see scripts/install-desktop-entry.sh
# for why debug and release are not interchangeable: the debug binary has
# devUrl http://localhost:1421 baked in and fails standalone with "Could not
# connect to localhost: Connection refused").
#
# ./start.sh              build if needed, then run the release binary
# ./start.sh --dev        npm run tauri dev (Vite HMR on :1421 + debug binary)
# ./start.sh --no-build   run the existing release binary, skip the build step
# ./start.sh -- <args>    everything after -- is forwarded to the launcher binary
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

mode="release"
no_build=0
passthrough=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dev)
      mode="dev"
      shift
      ;;
    --no-build)
      no_build=1
      shift
      ;;
    --)
      shift
      passthrough=("$@")
      break
      ;;
    *)
      echo "unknown argument: $1" >&2
      echo "usage: $0 [--dev] [--no-build] [-- args-for-launcher]" >&2
      exit 1
      ;;
  esac
done

for tool in npm cargo; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool not found on PATH, required to run the launcher." >&2
    exit 1
  fi
done

if [[ ! -d node_modules ]]; then
  echo "node_modules missing, running npm ci first"
  npm ci
fi

emulator_hint="../../_Build/linux/install/kyty_emulator"
if [[ ! -x "$emulator_hint" ]]; then
  echo "warning: no kyty_emulator found at $emulator_hint" >&2
  echo "         the launcher UI will still start, but pressing Launch will" >&2
  echo "         fail with \"Could not find kyty_emulator\" until the emulator is built." >&2
fi

if [[ "$mode" == "dev" ]]; then
  if command -v ss >/dev/null 2>&1; then
    holder="$(ss -ltnp 'sport = :1421' 2>/dev/null | awk 'NR>1')"
    if [[ -n "$holder" ]]; then
      echo "Port 1421 is already in use, so a second Vite dev server can't start." >&2
      echo "That means this run stops here instead of launching the UI; nothing else is affected." >&2
      echo "What is holding it:" >&2
      echo "  $holder" >&2
      echo "Stop it first, e.g.: kill \$(ss -ltnp 'sport = :1421' | awk 'NR>1 {print \$NF}' | grep -oP '(?<=pid=)\\d+')" >&2
      exit 1
    fi
  fi
  exec npm run tauri dev -- "${passthrough[@]}"
fi

bin="src-tauri/target/release/kyty-launcher"

if [[ $no_build -eq 1 ]]; then
  if [[ ! -x "$bin" ]]; then
    echo "error: $bin does not exist and --no-build was passed." >&2
    echo "       build it first: npm run tauri build -- --no-bundle" >&2
    exit 1
  fi
else
  npm run tauri build -- --no-bundle
fi

exec "$bin" "${passthrough[@]}"
