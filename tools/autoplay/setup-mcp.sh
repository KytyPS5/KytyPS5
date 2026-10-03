#!/bin/sh
# One-time setup for the kyty MCP server: a private virtualenv with its dependencies, and a check
# that Ollama has the vision model. Safe to run again. Works from any shell: sh tools/autoplay/setup-mcp.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
venv="$here/.venv"
model="${KYTY_VISION_MODEL:-qwen2.5vl:3b}"
host="${OLLAMA_HOST:-http://localhost:11434}"

if [ ! -x "$venv/bin/python" ]; then
    echo "Creating $venv"
    if ! python3 -m venv "$venv"; then
        echo "Could not create a virtualenv. On Debian/Ubuntu: sudo apt install python3-venv" >&2
        echo "(Fedora: sudo dnf install python3; Arch: python is enough.)" >&2
        exit 1
    fi
fi

echo "Installing $here/requirements-mcp.txt"
"$venv/bin/python" -m pip install --quiet --upgrade pip || exit 1
"$venv/bin/python" -m pip install --quiet -r "$here/requirements-mcp.txt" || exit 1
"$venv/bin/python" -c "import mcp, PIL" || { echo "The packages installed but do not import." >&2; exit 1; }
echo "MCP server ready: sh $here/mcp_server.sh"

# Ollama is optional for starting the server, but `look` needs it.
if command -v curl >/dev/null 2>&1; then
    tags=$(curl -s --max-time 5 "$host/api/tags" 2>/dev/null)
    if [ -z "$tags" ]; then
        echo "Ollama is not reachable at $host: install it from https://ollama.com and run 'ollama serve'."
    elif ! printf '%s' "$tags" | grep -q "\"$model\""; then
        echo "Ollama has no $model yet: run 'ollama pull $model'."
    else
        echo "Ollama has $model."
    fi
else
    echo "Check Ollama yourself: 'ollama list' should show $model (or run 'ollama pull $model')."
fi
echo "Then check that 'claude mcp list' shows kyty as connected."
