#!/bin/sh
# Translate prototypeenginef.dll to LLVM IR (work/engine/engine.ll).
# See translate_module.sh for the steps.
set -eu
exec sh "$(dirname "$0")/translate_module.sh" engine prototypeenginef.dll \
    d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071 10001000 10D8F364
