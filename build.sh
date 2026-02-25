#!/bin/sh
# Bootstrap: compile build.c into ./builder
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
./tcc -Blib/tcc-linux -o builder build.c
