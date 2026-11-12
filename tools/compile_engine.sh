#!/bin/sh
# Compile work/engine/engine.ll (the llasm output) to native ARM64 objects.
#
# Steps:
#   1. opt sets the arm64-apple-macosx target triple and data layout, checks
#      the IR with the LLVM verifier, and writes bitcode.
#   2. llvm-split divides the module into PARTS modules, so that the parts
#      compile in parallel and each compiler process uses less memory.
#   3. tools/build_sse_inline.sh compiles the SSE helpers (the sse_*
#      functions of llasm_x86ext.c) to bitcode. llvm-link copies the helpers
#      that a part calls into that part as internal functions, so that clang
#      can inline them. The bundle also links llasm_x86ext.o, which keeps
#      the external helpers. The release converter does the same step.
#   4. clang compiles each part with -O2 to a position-independent ARM64
#      object (upstream SR uses --relocation-model=pic for arm64-darwin).
#
# Run from the mac-port directory, after tools/translate_engine.sh.
# The objects contain the game's code. Do not distribute them.
set -eu

PORT_DIR=$(cd "$(dirname "$0")/.." && pwd)
WORK="$PORT_DIR/work/engine"
LLVM=/opt/homebrew/opt/llvm/bin
TRIPLE=arm64-apple-macosx14.0.0
DATALAYOUT='e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32'
PARTS=${PARTS:-96}
JOBS=${JOBS:-10}

cd "$WORK"

echo "PROGRESS verify and convert to bitcode"
"$LLVM/opt" --mtriple=$TRIPLE --data-layout="$DATALAYOUT" engine.ll -o engine.bc

echo "PROGRESS split into $PARTS parts"
rm -rf parts
mkdir parts
"$LLVM/llvm-split" -j "$PARTS" engine.bc -o parts/engine

echo "PROGRESS compile the SSE helpers to bitcode"
sh "$PORT_DIR/tools/build_sse_inline.sh" parts/sse_inline.bc

echo "PROGRESS compile parts with $JOBS jobs"
ls parts | grep -E '^engine[0-9]+$' | sort -n -k1.7 > parts/list
count=$(wc -l < parts/list | tr -d ' ')
if [ "$count" -ne "$PARTS" ]; then
    echo "error: llvm-split wrote $count parts, expected $PARTS" >&2
    exit 1
fi
xargs -P "$JOBS" -I{} sh -c \
    '{ "$0/llvm-link" --only-needed --internalize "parts/$2" parts/sse_inline.bc -o "parts/$2.sse.bc" && "$0/clang" -target "$1" -O2 -fPIC -c -x ir "parts/$2.sse.bc" -o "parts/$2.o"; } 2> "parts/$2.log" || { echo "error: part $2 failed:" >&2; head -20 "parts/$2.log" >&2; exit 255; }' \
    "$LLVM" "$TRIPLE" {} < parts/list

for p in $(cat parts/list); do
    if [ -s "parts/$p.log" ]; then
        echo "warning output from part $p:" >&2
        head -20 "parts/$p.log" >&2
    fi
done
echo "PROGRESS done: $(cat parts/*.o | wc -c | tr -d ' ') bytes of ARM64 objects"
