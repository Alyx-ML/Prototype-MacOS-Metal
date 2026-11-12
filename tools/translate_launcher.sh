#!/bin/sh
# Translate prototypef.exe to LLVM IR (work/launcher/launcher.ll).
# See translate_module.sh for the steps. The file has no relocation table.
# work/launcher/relocations.csv comes from tools/gen_relocations.py (the
# candidate 0x405074 is a false positive inside a type name string and is
# removed).
set -eu
exec sh "$(dirname "$0")/translate_module.sh" launcher prototypef.exe \
    ac1542512ab5eddffcbf949919f3dcc4b26e38f07b57cb6a094def2a621b0b82 401000 402C3C
