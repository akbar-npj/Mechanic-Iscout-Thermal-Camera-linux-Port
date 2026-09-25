#!/usr/bin/env python3
"""
extract_mnn_model.py — recover the super-resolution model embedded in
libmnnmodel.so.

The Android app ships a 2x super-resolution model (MImageUtils.ModelName.
MNN_ZOOM_X2) but **no model file**: there is nothing in the APK's assets, and
`native_init_mnn_model_module(Context)` turns out to be a 40-byte no-op that
sets a flag and ignores its Context argument.  The weights are instead embedded,
obfuscated, inside libmnnmodel.so itself, and this script re-derives them so the
result is reproducible rather than a checked-in blob of unknown origin.

The mechanism, from the arm64-v8a disassembly (RE Docs 09 §6):

  native_mnn_run_1(env, this, model, level)
      -> mnn_run_1(1, level)
          -> sr1(level, key)                       @ 0x6010
                 buf = operator new[](size)        @ 0x6034-0x6044
                 for i in 0..size:                 @ 0x605c-0x60b8
                     buf[i] = .data[i] ^ key[i % 25]
                 MNN::Interpreter::createFromBuffer(buf, size)   @ 0x60cc

so:

  * the ciphertext is the whole of .data from VA 0xf120 (the section's start),
  * its length is a uint32 global at VA 0x123a0 (the last word of .data),
  * the key is 25 bytes at .rodata VA 0x22e0 (a 32-byte table; only 25 are used),
  * the plaintext is an MNN flatbuffer — the file MNN's createFromBuffer wants.

The three addresses are the only hard-coded values, and each is asserted to fall
inside the section it is expected in, so a changed .so fails loudly instead of
producing a plausible-looking wrong file.

Usage:
    python3 tools/extract_mnn_model.py \\
        ../RE\\ Workspace/apk/lib/arm64-v8a/libmnnmodel.so ../linux-port/models
"""

import argparse
import hashlib
import os
import struct
import sys

# From the disassembly — see the module docstring for the instruction sites.
CIPHERTEXT_VA = 0xF120      # .data start; the buffer sr1 walks
SIZE_VA       = 0x123A0     # uint32: how many bytes to decrypt
KEY_VA        = 0x22E0      # 32-byte table; sr1 uses 25 of them
KEY_LEN       = 25

# The plaintext is the app's 2x zoom model.  These are the markers that say so:
# the flatbuffer's asset UUID and the tensors the graph exposes.
EXPECTED_UUID    = b"7006ec85-d318-4f47-9508-fbbe5f08ec82"
EXPECTED_TENSORS = (b"image_input", b"image_output")
EXPECTED_OPS     = (b"onnx::DepthToSpace_26", b"onnx::Conv_22", b"onnx::Clip_27")

OUT_NAME = "zoom2.mnn"


def parse_sections(blob):
    """Return {name: (addr, offset, size)} for every section header."""
    if blob[:4] != b"\x7fELF" or blob[4] != 2:      # ELF, 64-bit
        raise SystemExit("not a 64-bit ELF")

    e_shoff, = struct.unpack_from("<Q", blob, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    if e_shoff == 0 or e_shnum == 0:
        raise SystemExit("no section headers")

    raw = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        name, stype, flags, addr, off, size = struct.unpack_from("<IIQQQQ", blob, base)
        raw.append((name, addr, off, size))

    strtab_off = raw[e_shstrndx][2]
    out = {}
    for name, addr, off, size in raw:
        end = blob.index(b"\x00", strtab_off + name)
        nm = blob[strtab_off + name:end].decode("ascii", "replace")
        out[nm] = (addr, off, size)
    return out


def inside(sec_name, sections, va, length):
    """Assert [va, va+length) lies inside `sec_name`; return the file offset."""
    if sec_name not in sections:
        raise SystemExit("no %s section" % sec_name)
    addr, off, size = sections[sec_name]
    if va < addr or va + length > addr + size:
        raise SystemExit("VA 0x%x+%d is outside %s (0x%x..0x%x)"
                         % (va, length, sec_name, addr, addr + size))
    return off + (va - addr)


def validate(model):
    """Reject anything that is not the flatbuffer we expect."""
    if len(model) < 8:
        raise SystemExit("model too short")
    root, = struct.unpack_from("<I", model, 0)
    if not (4 <= root < len(model) - 4) or root % 4:
        raise SystemExit("implausible flatbuffer root offset 0x%x" % root)
    for what, needles in (("UUID", (EXPECTED_UUID,)),
                          ("tensor", EXPECTED_TENSORS),
                          ("op", EXPECTED_OPS)):
        for n in needles:
            if n not in model:
                raise SystemExit("decrypted model is missing the %s marker %r"
                                 % (what, n))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("so", help="libmnnmodel.so (arm64-v8a)")
    ap.add_argument("outdir", help="where to write zoom2.mnn")
    args = ap.parse_args()

    blob = open(args.so, "rb").read()
    sections = parse_sections(blob)

    size_off = inside(".data", sections, SIZE_VA, 4)
    size, = struct.unpack_from("<I", blob, size_off)
    print("ciphertext length (global @0x%x): %d bytes" % (SIZE_VA, size))
    if size == 0 or size > 0x1000000:
        raise SystemExit("implausible model size %d" % size)

    key_off = inside(".rodata", sections, KEY_VA, 32)
    key = blob[key_off:key_off + KEY_LEN]
    print("key (%d bytes @0x%x): %s" % (KEY_LEN, KEY_VA, key.hex()))

    ct_off = inside(".data", sections, CIPHERTEXT_VA, size)
    ct = blob[ct_off:ct_off + size]

    model = bytes(c ^ key[i % KEY_LEN] for i, c in enumerate(ct))
    validate(model)

    os.makedirs(args.outdir, exist_ok=True)
    path = os.path.join(args.outdir, OUT_NAME)
    with open(path, "wb") as f:
        f.write(model)

    print("wrote %s (%d bytes)" % (path, len(model)))
    print("sha256 %s" % hashlib.sha256(model).hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main())
