#!/bin/sh
# Translate one PE module to LLVM IR.
#
# Usage: translate_module.sh <work-dir-name> <pe-file> <sha256> <code-start> <code-end>
#   work-dir-name: directory under work/ (for example "engine")
#   pe-file: file name of the PE module in the work directory
#   sha256: expected SHA-256 of the PE module
#   code-start, code-end: virtual address range of the code section (hex)
# The output is work/<work-dir-name>/<work-dir-name>.ll.
#
# The script repeats these steps until no new information is found:
#   1. SRW translates the DLL to llasm.
#   2. fix_region_flags.py adds instruction_flags.sci entries for flag
#      values that are live across region starts.
#   3. find_code_seeds.py adds code addresses that only data references.
# Then it checks the flag entries against the SRW control flow, generates
# the data-only import declarations, and runs llasm.
# The llasm options are the upstream SR options for arm64-darwin with
# offset pointers (games/Septerra Core/SR-Septerra/SConstruct).
#
# The work directory must contain the PE file (or a link to it) and the
# .sci files.
set -eu

PORT_DIR=$(cd "$(dirname "$0")/.." && pwd)
if [ $# -ne 5 ]; then
    echo "usage: translate_module.sh <work-dir-name> <pe-file> <sha256> <code-start> <code-end>" >&2
    exit 2
fi
NAME=$1
PE=$2
EXPECTED_SHA256=$3
CODE_START=$4
CODE_END=$5
WORK="$PORT_DIR/work/$NAME"
SRW="$PORT_DIR/build/srw/SRW"
LLASM="$PORT_DIR/build/llasm/llasm"
TOOLS="$PORT_DIR/tools"
PY="python3 -I"
MAX_PASSES=32

cd "$WORK"

actual=$(shasum -a 256 $PE | cut -d' ' -f1)
if [ "$actual" != "$EXPECTED_SHA256" ]; then
    echo "error: $PE has SHA-256 $actual, expected $EXPECTED_SHA256" >&2
    exit 1
fi

pass=1
while [ $pass -le $MAX_PASSES ]; do
    echo "PROGRESS pass $pass: SRW"
    srw_rc=0
    "$SRW" $PE $NAME.llasm > out.log 2> err.log || srw_rc=$?
    changed=0
    if [ $srw_rc -ne 0 ]; then
        if grep -q '^Error: unknown flags on start of region' err.log; then
            fix_rc=0
            $PY "$TOOLS/fix_region_flags.py" $PE err.log instruction_flags.sci disasm.cache || fix_rc=$?
            if [ $fix_rc -eq 0 ]; then changed=1; else
                echo "error: fix_region_flags.py failed with status $fix_rc" >&2; exit 1; fi
        else
            echo "error: SRW failed with status $srw_rc:" >&2
            grep '^Error' err.log | head -20 >&2
            exit 1
        fi
    else
        seed_rc=0
        $PY "$TOOLS/find_code_seeds.py" seg01_code.llinc fixup_interpret_as_code.sci $CODE_START $CODE_END \
            $NAME.llasm seg*_data.llinc || seed_rc=$?
        if [ $seed_rc -eq 0 ]; then changed=1
        elif [ $seed_rc -ne 1 ]; then echo "error: find_code_seeds.py failed with status $seed_rc" >&2; exit 1; fi
    fi
    if [ $changed -eq 0 ]; then break; fi
    pass=$((pass + 1))
done
if [ $pass -gt $MAX_PASSES ]; then
    echo "error: no convergence after $MAX_PASSES passes" >&2
    exit 1
fi

echo "PROGRESS verify flag entries"
$PY "$TOOLS/verify_region_flags.py" seg01_code.llinc instruction_flags.sci disasm.cache 2> verify.log

echo "PROGRESS declare data-only imports"
$PY "$TOOLS/gen_extern_llinc.py" "$PORT_DIR/runtime/imports.spec" $NAME.llasm extern.llinc \
    seg*_data.llinc

echo "PROGRESS llasm"
"$LLASM" -m64 -inline-idiv -inline-float -pic -ptrofs -O -I "$PORT_DIR/third_party/SR/SR/llasm-support" -I "$PORT_DIR/runtime/llasm" -I . \
    $NAME.llasm -o $NAME.ll > llasm.out 2> llasm.err || {
    echo "error: llasm failed:" >&2
    grep -v '^Replacing' llasm.err | head -20 >&2
    exit 1
}
echo "PROGRESS done: $(ls -l $NAME.ll | awk '{print $5}') bytes of LLVM IR"
