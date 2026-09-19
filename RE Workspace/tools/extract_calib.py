#!/usr/bin/env python3
"""
extract_calib.py — pull the Windows calibration tables out of the NSIS block dump.

The four calibration tables the Windows build ships are stored, unnamed, among
the ~200 data blocks that nsis_extract.py produces.  Their identities were
recovered by disassembling the loaders (see RE Docs 10-calibration-tables.md);
this script re-derives them from the bytes so the mapping is reproducible
rather than a hard-coded copy.

Identification is structural, not positional:

  tau_H.bin / tau_L.bin   exactly 7168 bytes  = uint16[56][64]  (no header)
  MILI6_*.bin             exactly 7424 bytes  = 256-byte header + uint16[56][64]

The MILI6 header is `uint32 version_and_flags` followed by 63 x 0xFFFFFFFF.
A block is only accepted if its bytes match one of those two shapes, so the
script fails loudly if the installer changes.

Usage:
    python3 tools/extract_calib.py exe/nsis_blocks ../linux-port/calib
"""

import argparse
import hashlib
import os
import struct
import sys

TAU_BYTES = 56 * 64 * 2          # 7168
MILI6_HEADER = 256
MILI6_BYTES = MILI6_HEADER + TAU_BYTES   # 7424


def classify(blob):
    """Return ('tau'|'mili6', gain_flag) or None."""
    if len(blob) == TAU_BYTES:
        # No header.  Row-major uint16[56][64]; the nearest-distance column of
        # every row is exactly 1.0 in Q14 (16384) because at 0.25 m there is
        # no atmosphere to attenuate the signal.
        if all(struct.unpack_from("<H", blob, (row * 64) * 2)[0] == 16384
               for row in range(56)):
            return ("tau", None)
        return None
    if len(blob) == MILI6_BYTES:
        dword0 = struct.unpack_from("<I", blob, 0)[0]
        if blob[4:MILI6_HEADER] == b"\xff" * (MILI6_HEADER - 4):
            return ("mili6", dword0 & 0xFFFF)
        return None
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blockdir", help="directory holding blkNNN.bin from nsis_extract.py")
    ap.add_argument("outdir", help="where to write tau_*.bin / MILI6_*.bin")
    args = ap.parse_args()

    found = {"tau": [], "mili6": []}
    for name in sorted(os.listdir(args.blockdir)):
        if not (name.startswith("blk") and name.endswith(".bin")):
            continue
        with open(os.path.join(args.blockdir, name), "rb") as f:
            blob = f.read()
        c = classify(blob)
        if c is None:
            continue
        kind, gain = c
        found[kind].append((name, gain, blob))

    ok = True
    if len(found["tau"]) != 2:
        print("ERROR: expected 2 tau blocks, found %d (%s)"
              % (len(found["tau"]), [n for n, _, _ in found["tau"]]), file=sys.stderr)
        ok = False
    if len(found["mili6"]) != 2:
        print("ERROR: expected 2 MILI6 blocks, found %d (%s)"
              % (len(found["mili6"]), [n for n, _, _ in found["mili6"]]), file=sys.stderr)
        ok = False
    if not ok:
        return 1

    # Within each family the two blocks differ only in gain.  The high-gain
    # table is the one that decays less with distance, which is also the one
    # whose MILI6 header has bit 0 set (RE Docs 10 §5).  Sort by the header
    # bit for MILI6 and by that same convention for tau.
    mili6_h = [b for n, g, b in found["mili6"] if g == 1]
    mili6_l = [b for n, g, b in found["mili6"] if g == 0]
    if len(mili6_h) != 1 or len(mili6_l) != 1:
        print("ERROR: MILI6 gain flags are not one 1 and one 0", file=sys.stderr)
        return 1

    # tau has no header, so order it by the same rule the MILI6 header states:
    # high gain = larger transmittance in the last distance column.
    tau_sorted = sorted(found["tau"], key=lambda t: struct.unpack_from("<H", t[2], (56 - 1) * 64 * 2 + 63 * 2)[0])
    tau_l, tau_h = tau_sorted[0], tau_sorted[1]

    os.makedirs(args.outdir, exist_ok=True)
    plan = [
        ("tau_H.bin", tau_h[2], tau_h[0]),
        ("tau_L.bin", tau_l[2], tau_l[0]),
        ("MILI6_H.bin", mili6_h[0], [n for n, g, _ in found["mili6"] if g == 1][0]),
        ("MILI6_L.bin", mili6_l[0], [n for n, g, _ in found["mili6"] if g == 0][0]),
    ]
    for out, blob, src in plan:
        path = os.path.join(args.outdir, out)
        with open(path, "wb") as f:
            f.write(blob)
        print("%-14s <- %-9s %6d bytes  sha256=%s"
              % (out, src, len(blob), hashlib.sha256(blob).hexdigest()[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
