#!/bin/sh
# Build the builder using TCC (platform-detected)

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

case "$(uname -s)" in
    Darwin)
        lib/tcc/macos/tcc -Blib/tcc/macos -o builder build.c && ./builder
        ;;
    Linux)
        ./tcc -Blib/tcc-linux -o builder build.c && ./builder
        ;;
    *)
        echo "Use build.bat on Windows"
        exit 1
        ;;
esac
