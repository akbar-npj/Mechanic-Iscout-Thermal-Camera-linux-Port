#!/bin/bash
GH=/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless
# Derived from this script's own location (<workspace>/ghidra/run.sh), so the
# workspace can be moved without editing this file.  It used to be a hardcoded
# absolute path into a different project's tree, which silently broke it.
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$BASE/ghidra/proj"
SCRIPTS="$BASE/ghidra/scripts"
OUT="$BASE/ghidra/out"
LIBS="$BASE/apk/lib/arm64-v8a"
for lib in libthermometry.so libuvc.so; do
  echo "########## $lib ##########"
  "$GH" "$PROJ" ThermalCam -import "$LIBS/$lib" -scriptPath "$SCRIPTS" \
     -postScript ExportDecompiled.java "$OUT/${lib}.c" -analysisTimeoutPerFile 900 2>&1
  echo "exit=$?"
done
