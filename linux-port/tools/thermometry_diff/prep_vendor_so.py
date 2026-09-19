#!/usr/bin/env python3
"""
prep_vendor_so.py — make a COPY of a vendor Android .so loadable by glibc.

The only fix applied
--------------------
The vendor libraries carry a **NULL entry in `.init_array`** (and a NULL second
entry in `.fini_array`), with no relocation to fill it in.  bionic skips NULL
entries when walking those arrays; glibc does not, so `dlopen()` segfaults by
jumping to address 0:

    Program received signal SIGSEGV, Segmentation fault.
    0x0000000000000000 in ?? ()
    #0  0x0000000000000000 in ?? ()
    #1  call_init (l=<optimized out>, ...) at dl-init.c:74

This is a property of the vendor's build, not of the local copy: the x86_64
variant of the same library has the same shape (a `.init_array` with no
relocation covering it), so it is not evidence of tampering.

Since the array's only entry is NULL, the library has no constructor at all.
Setting `DT_INIT_ARRAYSZ` / `DT_FINI_ARRAYSZ` to zero is therefore behaviourally
identical to what bionic does, and loses nothing.  Destructors are irrelevant to
the numerical results this harness exists to produce.

Note: symbol versioning is NOT patched here.  The bionic `LIBC` version node is
supplied by the shim libraries in `shim/`, which keeps this file otherwise
byte-identical to the vendor artifact.

usage: prep_vendor_so.py <input.so> <output.so>
"""

import struct
import sys

SHT_DYNAMIC = 6
PT_LOAD = 1

DT_INIT_ARRAY = 0x19
DT_INIT_ARRAYSZ = 0x1B
DT_FINI_ARRAY = 0x1A
DT_FINI_ARRAYSZ = 0x1C

SZ_NAMES = {DT_INIT_ARRAYSZ: "DT_INIT_ARRAYSZ", DT_FINI_ARRAYSZ: "DT_FINI_ARRAYSZ"}
PTR_NAMES = {DT_INIT_ARRAY: "DT_INIT_ARRAY", DT_FINI_ARRAY: "DT_FINI_ARRAY"}


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2

    src, dst = argv[1], argv[2]
    with open(src, "rb") as f:
        b = bytearray(f.read())

    if b[:4] != b"\x7fELF" or b[4] != 2:
        sys.stderr.write("only ELF64 is supported: %s\n" % src)
        return 1

    e_phoff, = struct.unpack_from("<Q", b, 0x20)
    e_shoff, = struct.unpack_from("<Q", b, 0x28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", b, 0x36)
    e_shentsize, e_shnum = struct.unpack_from("<HH", b, 0x3A)

    loads = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", b, o)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_offset, p_filesz))

    def va_to_off(va):
        for vaddr, off, filesz in loads:
            if vaddr <= va < vaddr + filesz:
                return off + (va - vaddr)
        return None

    # Walk .dynamic and collect the entries we care about.
    entries = []          # (file_offset_of_dyn_entry, tag, value)
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from("<I", b, o + 4)
        if sh_type != SHT_DYNAMIC:
            continue
        sh_offset, sh_size = struct.unpack_from("<QQ", b, o + 0x18)
        for k in range(sh_size // 16):
            eo = sh_offset + k * 16
            tag, val = struct.unpack_from("<qQ", b, eo)
            if tag == 0:
                break
            entries.append((eo, tag, val))

    changed = 0
    for eo, tag, val in entries:
        if tag not in SZ_NAMES:
            continue

        # Find the matching pointer tag to locate the array.
        ptr_tag = DT_INIT_ARRAY if tag == DT_INIT_ARRAYSZ else DT_FINI_ARRAY
        ptr = next((v for _, t, v in entries if t == ptr_tag), None)
        n = val // 8

        verdict = ""
        if ptr is not None and n:
            arr_off = va_to_off(ptr)
            if arr_off is not None:
                words = struct.unpack_from("<%dQ" % n, b, arr_off)
                nulls = sum(1 for w in words if w == 0)
                verdict = "  [%d of %d entries are NULL]" % (nulls, n)

        print("  %-16s = %d bytes%s" % (SZ_NAMES[tag], val, verdict))

        if val:
            struct.pack_into("<Q", b, eo + 8, 0)
            print("      -> set to 0 (bionic skips NULL entries; glibc jumps to 0)")
            changed += 1

    if not changed:
        print("  nothing to patch — no init/fini array size to clear")

    with open(dst, "wb") as f:
        f.write(bytes(b))
    print("  wrote %s (%d bytes)" % (dst, len(b)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
