#!/bin/bash
# run_win.sh — Ghidra headless analysis of the Windows native DLLs.
#
# These are the x86 counterparts of the Android ARM64 libraries: the
# Windows build ships libirtemp.dll (the transmittance/tau model) and
# Temperature.dll, plus the transport DLLs.  The point of decompiling
# them is to find the loaders for the Windows-only calibration tables
# (tau_*.bin, MILI6_*.bin, block_lut.dat) — the last substantive unknown
# in the Linux port (RE Docs 09 §3).
#
# Uses a separate Ghidra project from the ARM64 work so the two
# architectures do not mix.
set -u
GH=/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless
# Derived from this script's own location (<workspace>/ghidra/run_win.sh);
# see run.sh.
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$BASE/ghidra/win/proj"
SCRIPTS="$BASE/ghidra/scripts"
OUT="$BASE/ghidra/win/out"
DLLS="$BASE/exe/win_dlls"

mkdir -p "$OUT" "$PROJ"

for dll in "$@"; do
  echo "########## $dll ##########"
  "$GH" "$PROJ" ThermalCamWin -import "$DLLS/$dll" \
        -scriptPath "$SCRIPTS" \
        -postScript ExportDecompiled.java "$OUT/${dll}.c" \
        -analysisTimeoutPerFile 900 2>&1 | tail -12
  echo "exit=$?"
done
echo "ALL DONE"
