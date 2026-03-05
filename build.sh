#!/bin/sh
# Bootstrap: compile build.c into ./builder
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ "$(uname -s)" != "Linux" ]; then
    echo "build.sh is Linux-only. Use build.cmd on macOS or build.bat on Windows."
    exit 1
fi

# Kill any running instances so the binary can be overwritten
killall AnitraEngine 2>/dev/null
killall collab_server 2>/dev/null
killall builder 2>/dev/null

./tcc -Blib/tcc-linux -o builder build.c
