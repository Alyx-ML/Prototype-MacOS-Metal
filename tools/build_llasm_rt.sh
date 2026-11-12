#!/bin/sh
# Translate runtime llasm files to ARM64 objects in build/rt.
#
# Usage: build_llasm_rt.sh <name>...
#   name: base name of a file in runtime/llasm (for example "calls").
#
# Each file goes through the same steps as the game code: llasm with the
# upstream SR options for arm64-darwin with offset pointers, then opt (target
# triple, data layout and the LLVM verifier), then clang -O2 -fPIC.
# The output is build/rt/<name>.o. The llasm messages are in
# build/rt/<name>.llasm.log.
set -eu

PORT_DIR=$(cd "$(dirname "$0")/.." && pwd)
LLASM="$PORT_DIR/build/llasm/llasm"
LLVM=/opt/homebrew/opt/llvm/bin
TRIPLE=arm64-apple-macosx14.0.0
DATALAYOUT='e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32'
OUT="$PORT_DIR/build/rt"

if [ $# -eq 0 ]; then
    echo "usage: build_llasm_rt.sh <name>..." >&2
    exit 2
fi
mkdir -p "$OUT"
for name in "$@"; do
    src="$PORT_DIR/runtime/llasm/$name.llasm"
    if [ ! -f "$src" ]; then
        echo "error: $src does not exist" >&2
        exit 1
    fi
    # llasm finds included files relative to the current directory and the
    # -I directories.
    (cd "$PORT_DIR/runtime/llasm" && "$LLASM" -m64 -inline-idiv -inline-float -pic -ptrofs -O \
        -I "$PORT_DIR/third_party/SR/SR/llasm-support" -I "$PORT_DIR/runtime/llasm" \
        "$name.llasm" -o "$OUT/$name.ll") > "$OUT/$name.llasm.log" 2>&1 || {
        echo "error: llasm failed for $name:" >&2
        grep -v '^Replacing' "$OUT/$name.llasm.log" | head -20 >&2
        exit 1
    }
    "$LLVM/opt" --mtriple=$TRIPLE --data-layout="$DATALAYOUT" "$OUT/$name.ll" -o "$OUT/$name.bc"
    "$LLVM/clang" -target $TRIPLE -O2 -fPIC -c -x ir "$OUT/$name.bc" -o "$OUT/$name.o"
done
