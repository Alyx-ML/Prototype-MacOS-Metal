#!/bin/sh
# Compile runtime C and Objective-C files (arguments: base names in
# runtime/src; a .m file is compiled with ARC) for a quick check. The complete build is in tools/build_app.sh.
set -eu
PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CC=/opt/homebrew/opt/llvm/bin/clang
mkdir -p "$PORT_DIR/build/rt"
for f in "$@"; do
    if [ -f "$PORT_DIR/runtime/src/$f.m" ]; then
        src="$PORT_DIR/runtime/src/$f.m"
        lang="-fobjc-arc"
    else
        src="$PORT_DIR/runtime/src/$f.c"
        lang="-std=c2x"
    fi
    "$CC" -target arm64-apple-macosx14.0.0 $lang -O2 -g \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror \
        -DPTROFS_64BIT -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" -isystem /opt/homebrew/opt/ffmpeg/include \
        -c "$src" -o "$PORT_DIR/build/rt/$f.o"
done
