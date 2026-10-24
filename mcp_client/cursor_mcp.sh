#!/bin/bash
# Cursor starts with a small PATH. Load nvm when `node` is not already visible.
if ! command -v node >/dev/null 2>&1; then
	if [ -s "$HOME/.nvm/nvm.sh" ]; then
		# nvm.sh is not POSIX, but it is the install this machine uses.
		# shellcheck disable=SC1091
		. "$HOME/.nvm/nvm.sh"
	fi
fi

if ! command -v node >/dev/null 2>&1; then
	echo "node was not found. Install it or start Cursor from a shell where node is on PATH." >&2
	exit 1
fi

exec node "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/cursor_mcp.mjs"
