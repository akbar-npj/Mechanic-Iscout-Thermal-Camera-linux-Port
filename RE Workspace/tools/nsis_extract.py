#!/usr/bin/env python3
"""
nsis_extract.py — minimal NSIS (Nullsoft Install System) extractor.

Written during the reverse-engineering of
  "iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe"
because 7-Zip 26.02's NSIS handler refused the file ("Is not archive")
even though the file is a genuine NSIS 3.07 installer.

It works; the format below was recovered empirically and cross-checked
against the published FirstHeader layout (NSIS Source/exehead/fileform.h).

------------------------------------------------------------
Format as observed
------------------------------------------------------------
Layout of the installer file:

    [PE stub]
    [FirstHeader]              <- 512-byte aligned, first thing in the overlay
    [compressed header block]
    [data blocks...]

FirstHeader (28 bytes, little-endian):

    +0x00 u32 flags
    +0x04 u32 siginfo            == 0xDEADBEEF
    +0x08 u32 "Null"             == 0x6C6C754E
    +0x0C u32 "soft"             == 0x74666F73
    +0x10 u32 "Inst"             == 0x74736E49
    +0x14 i32 length_of_header   (DECOMPRESSED header size)
    +0x18 i32 length_of_all_following_data

The FirstHeader is located at the first 512-byte-aligned offset at or after the
end of the last PE section with raw data.

Header block (immediately follows the FirstHeader, 8 bytes after it):

    +0x00 u32 size               total compressed size, INCLUDING the 5-byte props
    +0x04 u32 marker             0x80000000
    +0x08 u8  lzma props
    +0x09 u32 lzma dict size
    +0x0D ... LZMA1 raw stream, (size - 5) bytes

Data blocks (each):

    +0x00 u32 size
    +0x04 u32 marker
             marker == 0          -> STORED: `size` raw bytes follow at +0x08
             marker == 0x80000000 -> LZMA: 5-byte props/dict at +0x08, then
                                     (size - 5) bytes of raw LZMA1 stream

  NOTE: blocks are NOT solid — every block carries its own props byte.
  Advance for block i:  off += 8 + size

------------------------------------------------------------
Usage
------------------------------------------------------------
    python3 nsis_extract.py <installer.exe> <outdir> [--max-blocks N]

Outputs every block as blkNNN.bin plus blkNNN.<ext> when the type is
recognised, and writes manifest.txt.
"""

import argparse
import lzma
import os
import struct
import sys

FH_SIG = 0xDEADBEEF
FH_MAGIC = (0x6C6C754E, 0x74666F73, 0x74736E49)  # "Null", "soft", "Inst"
LZMA_MARKER = 0x80000000

MAGIC_EXT = [
    (b"MZ", "exe"),
    (b"%PDF", "pdf"),
    (b"BM", "bmp"),
    (b"\x89PNG", "png"),
    (b"RIFF", "wav"),
    (b"PK\x03\x04", "zip"),
]


def find_first_header(data):
    """Return the file offset of the NSIS FirstHeader, or None.

    The published FH_FLAGS_MASK (0x0F) is an NSIS 2.x constraint and does NOT
    hold for NSIS 3.x (this installer has flags=0x50), so flags are not used as
    a validity filter. Instead candidates are scored on the properties that
    actually distinguish a real FirstHeader from a coincidental byte match:

      * the FirstHeader is the first thing in the PE overlay at a
        512-byte-aligned offset                       (strongest signal)
      * length_of_all_following_data == fileSize - offset
      * the compressed header block that follows starts with the
        0x80000000 marker used by NSIS LZMA blocks

    A coincidental "NullsoftInst" string inside compressed payload data can
    satisfy some of these (one exists at 0x118c6e50 in this installer) but
    never the 512-byte alignment.
    """
    needle = struct.pack("<I", FH_SIG) + b"NullsoftInst"
    best = None
    best_score = -1
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        start = i + 1
        fh = i - 4
        if fh < 0 or fh + 36 > len(data):
            continue
        flags, sig, n0, n1, n2, hlen, allf = struct.unpack("<7I", data[fh:fh + 28])
        if sig != FH_SIG or (n0, n1, n2) != FH_MAGIC:
            continue
        score = 0
        if fh % 512 == 0:
            score += 4
        if allf == len(data) - fh:
            score += 2
        if 0 < hlen < (1 << 26):
            score += 1
        if struct.unpack("<I", data[fh + 32:fh + 36])[0] == LZMA_MARKER:
            score += 4
        if score > best_score:
            best_score = score
            best = fh
    if best is None:
        return None
    print("FirstHeader candidate @ %#x (score %d)" % (best, best_score))
    return best


def lzma_decompress(data, off, size):
    """Decompress one LZMA block located at `off` with total compressed size `size`."""
    props = data[off]
    dictsize = struct.unpack("<I", data[off + 1:off + 5])[0]
    lc = props % 9
    rem = props // 9
    lp = rem % 5
    pb = rem // 5
    if lc > 4 or lp > 4 or pb > 4:
        raise ValueError("implausible LZMA props %#x" % props)
    filt = [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": lp, "pb": pb,
             "dict_size": dictsize}]
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt)
    return dec.decompress(data[off + 5:off + size])


def read_block(data, off):
    """Return (size, out_bytes, how, advance)."""
    size = struct.unpack("<I", data[off:off + 4])[0]
    marker = struct.unpack("<I", data[off + 4:off + 8])[0]
    if marker == 0:
        return size, data[off + 8:off + 8 + size], "stored", 8 + size
    if marker == LZMA_MARKER:
        return size, lzma_decompress(data, off + 8, size), "lzma", 8 + size
    raise ValueError("unknown block marker %#x at %#x" % (marker, off))


def guess_ext(blob):
    for sig, ext in MAGIC_EXT:
        if blob.startswith(sig):
            return ext
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("installer")
    ap.add_argument("outdir")
    ap.add_argument("--max-blocks", type=int, default=100000)
    ap.add_argument("--max-size", type=int, default=0,
                    help="skip writing blocks larger than this many bytes "
                         "(0 = no limit); the manifest still records them")
    args = ap.parse_args()

    with open(args.installer, "rb") as f:
        data = f.read()
    print("file size: %#x (%d bytes)" % (len(data), len(data)))

    fh = find_first_header(data)
    if fh is None:
        print("ERROR: no NSIS FirstHeader found", file=sys.stderr)
        return 2
    flags, sig, n0, n1, n2, hlen, allf = struct.unpack("<7I", data[fh:fh + 28])
    print("FirstHeader @ %#x  flags=%#x  decompressed header size=%d" % (fh, flags, hlen))

    # --- header block ---
    # NOTE: the block header does NOT start immediately after the 28-byte
    # FirstHeader. In this installer there are 8 further zero bytes at
    # fh+28..fh+35 (possibly a CRC field in NSIS 3.x), and the block header
    # begins at fh+36. Verified: fh=0x11600 -> block size field at 0x11624,
    # LZMA data at 0x11631, and data blocks begin at 0x13DE2.
    HB = fh + 36
    hsize = struct.unpack("<I", data[HB:HB + 4])[0]
    hmarker = struct.unpack("<I", data[HB + 4:HB + 8])[0]
    if hmarker != LZMA_MARKER:
        print("ERROR: header block marker %#x (expected %#x)"
              % (hmarker, LZMA_MARKER), file=sys.stderr)
        return 2
    hdr = lzma_decompress(data, HB + 8, hsize)
    print("header block: compressed=%d decompressed=%d" % (hsize, len(hdr)))
    if len(hdr) != hlen:
        print("WARNING: decompressed header size != length_of_header", file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    with open(os.path.join(args.outdir, "_nsis_header.bin"), "wb") as f:
        f.write(hdr)

    # --- data blocks ---
    off = HB + 8 + hsize
    manifest = []
    i = 0
    while off + 8 <= len(data) and i < args.max_blocks:
        try:
            size, blob, how, adv = read_block(data, off)
        except Exception as e:
            manifest.append((i, off, None, None, how if False else "FAIL",
                             "ERROR: %s" % e))
            print("block %d @ %#x: FAILED: %s" % (i, off, e), file=sys.stderr)
            break
        ext = guess_ext(blob)
        name = "blk%03d.bin" % i
        if not args.max_size or len(blob) <= args.max_size:
            with open(os.path.join(args.outdir, name), "wb") as f:
                f.write(blob)
            if ext:
                with open(os.path.join(args.outdir, "blk%03d.%s" % (i, ext)), "wb") as f:
                    f.write(blob)
        manifest.append((i, off, size, len(blob), how, ext or ""))
        off += adv
        i += 1

    with open(os.path.join(args.outdir, "manifest.txt"), "w") as f:
        for row in manifest:
            f.write("blk%03d off=%#010x comp=%s size=%s %s %s\n"
                    % (row[0], row[1], row[2], row[3], row[4], row[5]))
        f.write("TOTAL BLOCKS: %d, end offset %#x, file size %#x\n"
                % (i, off, len(data)))
    print("extracted %d blocks -> %s" % (i, args.outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
