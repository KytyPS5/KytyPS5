#!/bin/sh
# Starts the kyty MCP server (mcp_server.py) with the project virtualenv made by setup-mcp.sh, or
# with python3 when there is none. Used by .mcp.json; works from any working directory.
here=$(cd "$(dirname "$0")" && pwd)
python="$here/.venv/bin/python"
[ -x "$python" ] || python=python3
if ! "$python" -c "import mcp" >/dev/null 2>&1; then
    echo "kyty MCP server: the mcp package is not installed for $python." >&2
    echo "Run: sh $here/setup-mcp.sh" >&2
    exit 1
fi
exec "$python" "$here/mcp_server.py" "$@"
