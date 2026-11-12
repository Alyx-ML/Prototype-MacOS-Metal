#!/bin/sh
# Make the SSE helper bitcode that the engine parts link before they are
# compiled (tools/compile_engine.sh and the release converter).
#
# Steps:
#   1. clang compiles llasm_x86ext.c to bitcode.
#   2. llvm-extract keeps the sse_* functions and the functions that they
#      call.
#   3. The script stops with an error if the result has global data:
#      each engine part gets its own internal copy of the helpers, and a
#      copy of global data in each part would be incorrect.
#   4. The target-cpu, target-features and tune-cpu attributes are
#      removed. The translated functions have no such attributes, and the
#      inliner does not inline a callee that has target features that the
#      caller does not have. The parts are compiled for the same CPU.
#   5. The source file name is replaced with the base name, so that the
#      file holds no path of the build computer.
#
# The result holds only the helper code of the SR llasm support (MIT
# license). It holds no game code.
#
# Usage: sh tools/build_sse_inline.sh <output .bc file>
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LLVM=/opt/homebrew/opt/llvm/bin
TRIPLE=arm64-apple-macosx14.0.0
OUT="${1:?usage: build_sse_inline.sh <output .bc file>}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/sse_inline.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

"$LLVM/clang" -target $TRIPLE -O2 \
    -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror -DPTROFS_64BIT -std=c2x \
    -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" \
    -emit-llvm -c "$PORT_DIR/third_party/SR/SR/llasm-support/llasm_x86ext.c" -o "$TMP/x86ext.bc"
"$LLVM/llvm-extract" --rfunc='^sse_' --recursive "$TMP/x86ext.bc" -o "$TMP/sse_all.bc"
"$LLVM/llvm-dis" "$TMP/sse_all.bc" -o "$TMP/sse_all.ll"
if grep -q '^@' "$TMP/sse_all.ll"; then
    echo "error: the SSE helpers use global data; they cannot be copied into each part" >&2
    exit 1
fi
if ! grep -q '^define .*@sse_' "$TMP/sse_all.ll"; then
    echo "error: llvm-extract found no sse_* functions in llasm_x86ext.c" >&2
    exit 1
fi
sed -E -e 's/ "(target-cpu|target-features|tune-cpu)"="[^"]*"//g' \
    -e 's/^source_filename = .*/source_filename = "llasm_x86ext.c"/' \
    -e 's/^; ModuleID = .*/; ModuleID = '\''sse_inline'\''/' \
    "$TMP/sse_all.ll" > "$TMP/sse_inline.ll"
if grep -q -e '"target-features"' -e '/Users/' "$TMP/sse_inline.ll"; then
    echo "error: the SSE helper bitcode still has target features or a build computer path" >&2
    exit 1
fi
"$LLVM/llvm-as" "$TMP/sse_inline.ll" -o "$OUT"
