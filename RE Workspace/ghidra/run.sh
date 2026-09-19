#!/bin/bash
GH=/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless
BASE="/home/shaanair/Projects/msm8916-openwrt-clean/GitIgnore/Thermal Camera/RE Workspace"
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
