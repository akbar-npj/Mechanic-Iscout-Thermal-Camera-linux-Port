#!/usr/bin/env python3
"""diff.py - byte-diff the portable C port against vendor ground truth.

For each <dir> under out/ (or the single dir given on the command line), this
compares the frozen vendor outputs against the port's outputs, byte for byte:

    in_lut_in.bin  vs  port_lut.bin      (the 16384-entry float LUT)
    out_image.bin  vs  port_out.bin      (10-header + width*(h-4) pixel floats)

Bytes are compared raw - NOT parsed as floats - so that NaN payloads, signed
zeros, and subnormals compare exactly as the vendor produced them.  A mismatch
reports the first differing byte offset, the surrounding float32 words on each
side, and a hex window so the regression can be localised.

The script also reads <dir>/meta.txt to print the case label (width, height,
fix_mode, sensor_mode) alongside each result, and to cross-check the port's
reported LUT endpoints (lut0 / lut_last) against the values the harness froze
into meta.txt - a float-level sanity check in addition to the byte compare.

usage:
    ./diff.py                 # scan every out/*/ directory
    ./diff.py out/256         # diff one case
    ./diff.py out/256 out/256_fixon   # diff specific cases

exit status: 0 iff every compared case is byte-identical (PASS); non-zero
otherwise.  This makes the script usable as a CI gate: a regression in port.c
turns the run red.

prereqs: the port must already have been run (./build/port <dir>) so that
port_lut.bin and port_out.bin exist.  The frozen vendor ground truth
(in_lut_in.bin, out_image.bin, meta.txt) is produced by run.sh + harness.
"""
import hashlib
import os
import struct
import sys


def sha256_hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def load(d: str, name: str):
    p = os.path.join(d, name)
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        return f.read()


def meta_get(d: str, key: str):
    """Read the first 'key <value>' line from <dir>/meta.txt, or None."""
    p = os.path.join(d, "meta.txt")
    if not os.path.exists(p):
        return None
    with open(p) as f:
        for line in f:
            line = line.strip()
            if line.startswith(key + " "):
                return line.split(None, 1)[1]
    return None


def fmt_float(b: bytes):
    """Render 4 bytes as '<float> (0x<hex>)' or '<short bytes>' if not 4-aligned."""
    if len(b) == 4:
        try:
            v = struct.unpack("<f", b)[0]
            if v != v:
                tag = "NaN"
            elif v == 0.0 and struct.unpack("<I", b)[0] & 0x80000000:
                tag = "-0.0"
            else:
                tag = repr(v)
            return f"{tag} (0x{b.hex()})"
        except struct.error:
            pass
    return f"0x{b.hex()}"


def cmp_pair(d: str, vendor_name: str, port_name: str, what: str):
    """Compare two files in <dir>.  Returns True=PASS, False=FAIL, None=SKIP."""
    v = load(d, vendor_name)
    p = load(d, port_name)
    if v is None and p is None:
        print(f"  [SKIP] {what}: neither vendor ({vendor_name}) nor port "
              f"({port_name}) present in {d}")
        return None
    if v is None:
        print(f"  [SKIP] {what}: no vendor ground truth ({vendor_name}) in {d}")
        return None
    if p is None:
        print(f"  [FAIL] {what}: no port output ({port_name}) in {d} - "
              f"run ./build/port {d} first")
        return False

    lv, lp = len(v), len(p)
    if v == p:
        print(f"  [PASS] {what}: {lv} bytes identical  sha256={sha256_hex(v)[:16]}...")
        return True

    # find the first differing byte
    n = min(lv, lp)
    i = 0
    while i < n and v[i] == p[i]:
        i += 1

    # align down to a 4-byte float32 boundary for a readable word
    off4 = i - (i % 4)
    vw = v[off4:off4 + 4] if off4 + 4 <= lv else v[off4:]
    pw = p[off4:off4 + 4] if off4 + 4 <= lp else p[off4:]

    print(f"  [FAIL] {what}: first diff at byte {i} (0x{i:x}); "
          f"len vendor={lv} port={lp}")
    print(f"         vendor [{off4:#x}..{off4+4:#x}] = {fmt_float(vw)}")
    print(f"         port   [{off4:#x}..{off4+4:#x}] = {fmt_float(pw)}")

    # show a small hex window around the divergence
    lo = max(0, off4 - 8)
    hi = min(n, off4 + 16)
    print(f"         byte window [0x{lo:x}..0x{hi:x}]:")
    print(f"           vendor: {v[lo:hi].hex()}")
    print(f"           port  : {p[lo:hi].hex()}")

    # if lengths match but bytes differ, also report the count of differing
    # float words so the scope of the regression is obvious
    if lv == lp:
        ndiff_words = sum(
            1 for k in range(off4 // 4, lv // 4)
            if v[k * 4:k * 4 + 4] != p[k * 4:k * 4 + 4]
        )
        print(f"         total differing float32 words from 0x{off4:x} "
              f"onward: {ndiff_words}")
    return False


def diff_dir(d: str):
    print(f"=== diff: {d} ===")
    w = meta_get(d, "width")
    h = meta_get(d, "total_height")
    fm = meta_get(d, "fix_mode")
    sm = meta_get(d, "sensor_mode")
    print(f"  meta: width={w} total_height={h} fix_mode={fm} sensor_mode={sm}")

    ok = True
    for what, vname, pname in (("LUT",   "in_lut_in.bin", "port_lut.bin"),
                               ("image", "out_image.bin", "port_out.bin")):
        r = cmp_pair(d, vname, pname, what)
        if r is False:
            ok = False

    # float-level sanity: the port's printed endpoints vs the harness's meta.
    # NB: compare float32 *bits*, not promoted doubles.  meta.txt stores
    # values with %.9g, which is enough to round-trip the float32 bits, but
    # parsing that string as a double yields a *different double* than
    # promoting the float32 to double - so `==` on doubles is unreliable.
    port_lut = load(d, "port_lut.bin")
    if port_lut is not None and len(port_lut) >= 8:
        lut0_words = len(port_lut) // 4
        lut0 = struct.unpack("<f", port_lut[0:4])[0]
        lut_last = struct.unpack("<f", port_lut[-4:])[0]
        m0, mlast = meta_get(d, "lut0"), meta_get(d, "lut_last")
        print(f"  port LUT endpoints: lut[0]={lut0:.9g}  "
              f"lut[{lut0_words - 1}]={lut_last:.9g}")
        if m0 is not None and mlast is not None:
            # re-quantize the meta decimal to float32 and compare raw bytes
            b0 = struct.pack("<f", float(m0))
            blast = struct.pack("<f", float(mlast))
            match = (b0 == port_lut[0:4] and
                     blast == port_lut[-4:])
            tag = "OK" if match else "MISMATCH"
            print(f"  meta LUT endpoints:  lut0={m0}  lut_last={mlast}  [{tag}]")
            if not match:
                ok = False

    print(f"  result: {'PASS' if ok else 'FAIL'}")
    print()
    return ok


def main():
    args = sys.argv[1:]
    if not args:
        # default: scan every out/*/ directory
        out_root = "out"
        if not os.path.isdir(out_root):
            print(f"no {out_root}/ directory here; pass a dir explicitly",
                  file=sys.stderr)
            return 2
        dirs = sorted(
            os.path.join(out_root, n)
            for n in os.listdir(out_root)
            if os.path.isdir(os.path.join(out_root, n))
        )
        if not dirs:
            print(f"no case directories under {out_root}/", file=sys.stderr)
            return 2
    else:
        dirs = args

    all_ok = True
    for d in dirs:
        if not os.path.isdir(d):
            print(f"=== {d}: not a directory ===", file=sys.stderr)
            all_ok = False
            continue
        if not diff_dir(d):
            all_ok = False

    print("=" * 60)
    print(f"OVERALL: {'PASS' if all_ok else 'FAIL'}  ({dirs})")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
