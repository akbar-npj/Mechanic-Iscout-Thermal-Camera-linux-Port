#!/usr/bin/env python3
"""dotnet_id.py - identify .NET assemblies by their CLR metadata Module name.

Used to attribute NSIS data blocks to real filenames. The NSIS string table is
indexed by first-use, NOT by File order, so block-index alignment drifts; the
internal Module name is authoritative.

Usage: python3 dotnet_id.py <dir-with-blkN.bin>
"""
import os, re, struct, sys

def pe_rva_to_off(b, va):
    e = struct.unpack_from('<I', b, 0x3c)[0]
    nsec = struct.unpack_from('<H', b, e+6)[0]
    optsz = struct.unpack_from('<H', b, e+20)[0]
    sec = e + 24 + optsz
    for i in range(nsec):
        o = sec + i*40
        vsz, vaddr, rsz, raddr = struct.unpack_from('<IIII', b, o+8)
        if vaddr <= va < vaddr + max(vsz, rsz):
            return raddr + (va - vaddr)
    return None

def clr_module_name(b):
    """Return the CLR Module name (table 0, row 0) or None."""
    if not b.startswith(b'MZ'):
        return None
    try:
        e = struct.unpack_from('<I', b, 0x3c)[0]
        if b[e:e+4] != b'PE\0\0':
            return None
        opt = e + 24
        magic = struct.unpack_from('<H', b, opt)[0]
        ddir = opt + (96 if magic == 0x10b else 112)
        clr_rva = struct.unpack_from('<I', b, ddir + 14*8)[0]
        if clr_rva == 0:
            return None
        co = pe_rva_to_off(b, clr_rva)
        if co is None:
            return None
        md_rva = struct.unpack_from('<I', b, co+8)[0]
        mo = pe_rva_to_off(b, md_rva)
        if mo is None or b[mo:mo+4] != b'BSJB':
            return None
        vlen = struct.unpack_from('<I', b, mo+12)[0]
        p = mo + 16 + vlen
        p += 2
        nstreams = struct.unpack_from('<H', b, p)[0]; p += 2
        strings_off = None; tables = None
        for _ in range(nstreams):
            so, ss = struct.unpack_from('<II', b, p)
            nm = b[p+8:b.index(b'\0', p+8)].decode('ascii', 'replace')
            if nm == '#Strings':
                strings_off = mo + so
            elif nm in ('#~', '#-'):
                tables = mo + so
            p += 8 + ((len(nm)//4)+1)*4
        if strings_off is None or tables is None:
            return None
        to = tables
        heapsizes = b[to+6]
        valid = struct.unpack_from('<Q', b, to+8)[0]
        rows_off = to + 24
        data_off = rows_off + 4*bin(valid).count('1')
        stridx = 4 if heapsizes & 0x01 else 2
        r = data_off                      # Module table is table 0
        name_idx = struct.unpack_from('<H' if stridx == 2 else '<I', b, r+2)[0]
        s = strings_off + name_idx
        return b[s:b.index(b'\0', s)].decode('utf-8', 'replace')
    except Exception:
        return None

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    for fn in sorted(os.listdir(d)):
        m = re.match(r'blk(\d+)\.bin$', fn)
        if not m:
            continue
        b = open(os.path.join(d, fn), 'rb').read()
        nm = clr_module_name(b)
        if nm:
            print("blk%03d %-9d %s" % (int(m.group(1)), len(b), nm))

if __name__ == '__main__':
    main()
