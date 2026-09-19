#!/bin/bash
GH=/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless
# Derived from this script's own location (<workspace>/ghidra/run2.sh); see run.sh.
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$BASE/ghidra/proj"; SCRIPTS="$BASE/ghidra/scripts"; OUT="$BASE/ghidra/out"
LIBS="$BASE/apk/lib/arm64-v8a"
echo "########## process libthermometry.so ##########"
"$GH" "$PROJ" ThermalCam -process libthermometry.so -noanalysis -scriptPath "$SCRIPTS" -postScript ExportDecompiled.java "$OUT/libthermometry.so.c" 2>&1 | tail -5
echo "########## process libuvc.so ##########"
"$GH" "$PROJ" ThermalCam -process libuvc.so -noanalysis -scriptPath "$SCRIPTS" -postScript ExportDecompiled.java "$OUT/libuvc.so.c" 2>&1 | tail -5
echo "########## import libDYTJpegAes.so ##########"
"$GH" "$PROJ" ThermalCam -import "$LIBS/libDYTJpegAes.so" -scriptPath "$SCRIPTS" -postScript ExportDecompiled.java "$OUT/libDYTJpegAes.so.c" 2>&1 | tail -5
echo "########## import libUVCCamera.so ##########"
"$GH" "$PROJ" ThermalCam -import "$LIBS/libUVCCamera.so" -scriptPath "$SCRIPTS" -postScript ExportDecompiled.java "$OUT/libUVCCamera.so.c" 2>&1 | tail -5
echo "ALL DONE"
