#!/usr/bin/env python3
"""pe_ident.py - identify native PE blocks via VS_VERSIONINFO.

For NSIS blocks that are PE but NOT CLR assemblies (so dotnet_id.py returns
nothing), the version resource carries OriginalFilename / FileDescription /
ProductName, which is authoritative.

Usage: python3 pe_ident.py <dir-with-blkN.bin>
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

def version_strings(b):
    """Return dict of version-resource string values."""
    if not b.startswith(b'MZ'):
        return {}
    out = {}
    try:
        e = struct.unpack_from('<I', b, 0x3c)[0]
        opt = e + 24
        magic = struct.unpack_from('<H', b, opt)[0]
        ddir = opt + (96 if magic == 0x10b else 112)
        rsrc_rva = struct.unpack_from('<I', b, ddir + 2*8)[0]
        if rsrc_rva == 0:
            return {}
        base = pe_rva_to_off(b, rsrc_rva)
        if base is None:
            return {}
        # resource dir: 16-byte header, then 8-byte entries
        def walk(off, depth, path):
            n_named, n_id = struct.unpack_from('<HH', b, off+12)
            for i in range(n_named + n_id):
                p = off + 16 + i*8
                name, sub = struct.unpack_from('<II', b, p)
                if name & 0x80000000:
                    continue
                if depth < 2:
                    walk(base + (sub & 0x7fffffff), depth+1, path+[name])
                else:
                    # leaf: data entry -> RVA+size
                    doff = base + sub
                    drva, dsz = struct.unpack_from('<II', b, doff)
                    fo = pe_rva_to_off(b, drva)
                    if fo is None:
                        continue
                    blob = b[fo:fo+dsz]
                    # scan UTF-16 key\0value\0 pairs, padded to 4 bytes
                    i2 = 0
                    while i2 + 6 < len(blob):
                        try:
                            end = blob.index(b'\0\0', i2)
                        except ValueError:
                            break
                        if (end - i2) % 2:
                            i2 += 2; continue
                        s = blob[i2:end+1].decode('utf-16-le', 'replace')
                        if '\0' not in s:
                            s = s + '\0'
                        parts = s.split('\0')
                        if len(parts) >= 2 and parts[0]:
                            out.setdefault(parts[0], parts[1] if len(parts) > 1 else '')
                        i2 = end + 1
                        while i2 < len(blob) and blob[i2] == 0:
                            i2 += 1
                        i2 = (i2 + 3) & ~3
        walk(base, 0, [])
    except Exception:
        pass
    return out

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    for fn in sorted(os.listdir(d)):
        m = re.match(r'blk(\d+)\.bin$', fn)
        if not m:
            continue
        b = open(os.path.join(d, fn), 'rb').read()
        if not b.startswith(b'MZ'):
            continue
        v = version_strings(b)
        if not v:
            continue
        orig = v.get('OriginalFilename', '')
        desc = v.get('FileDescription', '')
        prod = v.get('ProductName', '')
        ver  = v.get('FileVersion', '')
        print("blk%03d %-9d orig=%-28s ver=%-14s desc=%s | prod=%s"
              % (int(m.group(1)), len(b), orig, ver, desc, prod))

if __name__ == '__main__':
    main()
