#!/bin/sh
# Build the builder using TCC (platform-detected)

case "$(uname -s)" in
    Darwin)
        lib/tcc/macos/tcc -Blib/tcc/macos -o builder build.c
        ;;
    Linux)
        ./tcc -Blib/tcc-linux -o builder build.c
        ;;
    *)
        echo "Use build.bat on Windows"
        exit 1
        ;;
esac
