#!/usr/bin/env python3
"""
prep_vendor_so.py — make a COPY of a vendor Android .so loadable by glibc.

Five fixes are applied, all auto-detected.  Each is a property of the vendor's
build rather than of the local copy, and none changes what the library
computes.

Fix 1 — the NULL `.init_array` entry
------------------------------------
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

Fix 2 — segment alignment for any host page size
------------------------------------------------
A PT_LOAD segment must satisfy `p_vaddr ≡ p_offset (mod page)` and
`p_align ≡ 0 (mod page)`, or glibc refuses the file:

    ELF load command address/offset not page-aligned

Some vendor libraries were linked for 4 KiB pages (`p_align = 0x1000`), which is
fine on a 4 KiB host but rejected on a 16 KiB one — as `libDYTJpegAes.so` is on
this machine, while `libthermometry.so` (linked at 0x10000) loads unaided.

The fix **re-lays out file offsets and leaves every `p_vaddr` alone**.  That
matters: symbol values, relocations and `.dynamic` pointers are all virtual
addresses, so keeping the vaddrs fixed means nothing inside the library has to
be rewritten.  Only `p_offset` (for PT_LOAD/PT_DYNAMIC/PT_GNU_RELRO), each
section's `sh_offset`, `e_shoff`, and the LOAD `p_align` change.

**The target is a fixed 64 KiB, not the host's page size.**  A host-dependent
target would make the artifact differ from machine to machine, and one aligned
only for 16 KiB would still be rejected on a 64 KiB host.  Because 64 KiB is a
multiple of 4 KiB and 16 KiB, the result is congruent modulo all three, so the
same prepared file loads on every host — and it matches the alignment the
vendor's own `libthermometry.so` already uses.  The invariants are asserted
after the rewrite for 4 KiB, 16 KiB and 64 KiB.

Fix 3 — segment protections when two PT_LOADs share a page
----------------------------------------------------------
Fix 2 makes each segment *aligned*, but it cannot make them *page-disjoint*,
because `p_vaddr` is fixed.  `libDYTJpegAes.so`'s executable segment ends at
vaddr 0x52a20 and its writable segment starts at 0x53a20 — one 4 KiB page
apart.  On a 4 KiB host those never share a page; on a 16 KiB or 64 KiB host
they do, and glibc maps the shared page with the **later** segment's
protection.  The page holding the tail of `.plt` therefore ends up `rw-`
instead of `r-x`, and the first call through the PLT dies with

    SIGSEGV, si_code = SEGV_ACCERR (2), si_addr = <the PLT stub itself>

The fix widens a segment's flags to the union of its own and any earlier
segment it shares a page with, so the later mapping preserves everything the
earlier one needed.  For this library that means the writable segment becomes
`R|W|X`.  It also empties `PT_GNU_RELRO`: `_dl_protect_relro()` mprotects
`ALIGN_DOWN(relro_start, page)` onwards, and this library's RELRO starts inside
the page that holds `.plt`, so it would re-apply `PROT_READ` and undo the
widening.

Both changes are **loader-compatibility only**: they change how the artifact is
mapped, never what it computes.  The cost is that the writable segment stays
writable (RELRO is a hardening feature, not a semantic one) and is executable.
That is acceptable for a prepared copy whose only job is to produce ground
truth for a differential harness — and the harness itself is the check: if the
widening or the re-layout corrupted anything, the vendor's own output would
stop matching.

Fix 4 — eager binding (`BIND_NOW`)
----------------------------------
Android links with `-Wl,-z,now` (full RELRO), so every imported symbol is bound
when the library is loaded.  The shim libraries here are built on the opposite
assumption, stated in `libc.c`: "harnesses dlopen with RTLD_LAZY, so a symbol
that is imported but never called need not resolve".  Against a NOW library
that contract cannot hold, and glibc rejects the dlopen outright:

    undefined symbol: gettimeofday, version LIBC

This matters for the MNN stack specifically: `libMNN.so`, `libMNN_Express.so`,
`libmnnmodel.so` and the APK's `libc++_shared.so` are all NOW, and together they
import ~137 libc symbols the shim does not define — the entire locale and dirent
surface, most of pthread, and a dozen maths functions — none of which the
inference path touches.

Clearing the flag changes only *when* a symbol resolves, never what the library
computes.  A symbol that is genuinely needed still resolves; if one were
missing, the harness would fail loudly on the first call rather than silently
producing a wrong number, and the differential comparison is what would catch
it.

Fix 5 — PT_LOAD segments crowding one page
------------------------------------------
glibc refuses a layout in which **three or more** PT_LOAD segments share a
single memory page:

    libmnnmodel.so: ELF load command address/offset not page-aligned

The message is misleading — the segments' `p_vaddr` and `p_offset` are
perfectly congruent — but that is the check that fails.  `libmnnmodel.so` has
exactly that shape: its three PT_LOADs sit at vaddr 0x0, 0xdb70 and 0xf120, so
at 16 KiB or 64 KiB pages all three land in page 0.  Its siblings are fine,
because their segments overlap only pairwise.

The remedy is to merge each connected group of page-sharing segments into one
PT_LOAD, taking the union of their flags (`R|W|X`, the same widening fix 3
already applies to a shared page).  `p_vaddr` is never moved, so no relocation,
symbol or `.dynamic` pointer has to be rewritten, and libraries whose pages are
shared only pairwise are left untouched.  See `fix_crowded_pages` for the
experiment that established this.

Note: symbol versioning is NOT patched here.  The bionic `LIBC`/`LIBC_N` version
nodes are supplied by the shim libraries in `vendor_shim/`, which keeps this
file otherwise byte-identical to the vendor artifact.

usage: prep_vendor_so.py <input.so> <output.so>
"""

import os
import struct
import sys

SHT_DYNAMIC = 6
SHT_NOBITS = 8
PT_LOAD = 1
PT_DYNAMIC = 2
PT_GNU_RELRO = 0x6474E552

PF_X = 0x1
PF_W = 0x2
PF_R = 0x4

# Every page size Linux actually runs on.  One artifact has to be valid for all
# of them, so every check and every fix considers the largest.
PAGE_SIZES = (0x1000, 0x4000, 0x10000)

DT_INIT_ARRAY = 0x19
DT_INIT_ARRAYSZ = 0x1B
DT_FINI_ARRAY = 0x1A
DT_FINI_ARRAYSZ = 0x1C

SZ_NAMES = {DT_INIT_ARRAYSZ: "DT_INIT_ARRAYSZ", DT_FINI_ARRAYSZ: "DT_FINI_ARRAYSZ"}
PTR_NAMES = {DT_INIT_ARRAY: "DT_INIT_ARRAY", DT_FINI_ARRAY: "DT_FINI_ARRAY"}

# Program-header fields are all QWORDs after the two DWORDs, so a PT_LOAD entry
# is 0x38 bytes, p_flags sits at +0x04 and p_offset at +0x08.
PH_FLAGS = 0x04
PH_OFFSET = 0x08
PH_ALIGN = 0x30


class Elf(object):
    """The few ELF structures both fixes need."""

    def __init__(self, b):
        self.b = b
        self.e_phoff, = struct.unpack_from("<Q", b, 0x20)
        self.e_shoff, = struct.unpack_from("<Q", b, 0x28)
        self.e_phentsize, self.e_phnum = struct.unpack_from("<HH", b, 0x36)
        self.e_shentsize, self.e_shnum = struct.unpack_from("<HH", b, 0x3A)

    def ph(self):
        out = []
        for i in range(self.e_phnum):
            o = self.e_phoff + i * self.e_phentsize
            t, fl, off, va, pa, fsz, msz, al = struct.unpack_from("<IIQQQQQQ", self.b, o)
            out.append(dict(idx=i, o=o, type=t, flags=fl, off=off, vaddr=va,
                            paddr=pa, filesz=fsz, memsz=msz, align=al))
        return out

    def sh(self):
        out = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            name, typ, fl, addr, off, size = struct.unpack_from("<IIQQQQ", self.b, o)
            out.append(dict(idx=i, o=o, name=name, type=typ, addr=addr,
                            off=off, size=size))
        return out


def fix_init_arrays(b, e):
    """Fix 1: clear DT_INIT_ARRAYSZ / DT_FINI_ARRAYSZ."""
    loads = [(p["vaddr"], p["off"], p["filesz"]) for p in e.ph() if p["type"] == PT_LOAD]

    def va_to_off(va):
        for vaddr, off, filesz in loads:
            if vaddr <= va < vaddr + filesz:
                return off + (va - vaddr)
        return None

    entries = []
    for sh in e.sh():
        if sh["type"] != SHT_DYNAMIC:
            continue
        for k in range(sh["size"] // 16):
            eo = sh["off"] + k * 16
            tag, val = struct.unpack_from("<qQ", b, eo)
            if tag == 0:
                break
            entries.append((eo, tag, val))

    changed = 0
    for eo, tag, val in entries:
        if tag not in SZ_NAMES:
            continue
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
    return changed


def fix_page_alignment(b, e, page):
    """Fix 2: re-lay out file offsets so PT_LOAD is congruent for `page`.

    `page` is deliberately a fixed 64 KiB rather than the host's page size, so
    the prepared artifact is the same everywhere and also valid on hosts with a
    smaller or larger page.  Returns the new bytes (or the original if nothing
    needed to move)."""
    ph = e.ph()
    loads = [p for p in ph if p["type"] == PT_LOAD]
    if not loads:
        return b, 0

    def congruent(p):
        return (p["vaddr"] - p["off"]) % page == 0

    def aligned(p):
        return p["align"] <= 1 or p["align"] % page == 0

    if all(congruent(p) and aligned(p) for p in loads):
        print("  segments already satisfy a %d-byte page — nothing to re-lay out"
              % page)
        return b, 0

    # New file offsets, in ascending vaddr order, with every vaddr preserved.
    loads.sort(key=lambda p: p["vaddr"])
    cursor = 0
    for p in loads:
        want = p["off"] + ((p["vaddr"] - p["off"]) % page)
        p["newoff"] = max(want, cursor)
        cursor = p["newoff"] + p["filesz"]
        if p["newoff"] != p["off"]:
            print("  PT_LOAD vaddr %#x: file offset %#x -> %#x  (align %#x -> %#x)"
                  % (p["vaddr"], p["off"], p["newoff"], p["align"], page))

    # Everything past the last LOAD is not mapped at runtime (the section header
    # table, .comment, .shstrtab); it just moves along with it.
    tail_old = loads[-1]["off"] + loads[-1]["filesz"]
    tail_new = cursor
    tail = b[tail_old:]
    out = bytearray(tail_new + len(tail))

    for p in loads:
        out[p["newoff"]:p["newoff"] + p["filesz"]] = b[p["off"]:p["off"] + p["filesz"]]
    out[tail_new:] = tail

    def map_off(o):
        for p in loads:
            if p["off"] <= o < p["off"] + p["filesz"]:
                return p["newoff"] + (o - p["off"])
        if o >= tail_old:
            return tail_new + (o - tail_old)
        return o

    # Patch the program headers.  PT_DYNAMIC and PT_GNU_RELRO are sub-ranges of
    # a LOAD, so they follow it through the same mapping.
    for p in ph:
        if p["type"] not in (PT_LOAD, PT_DYNAMIC, PT_GNU_RELRO):
            continue
        struct.pack_into("<Q", out, p["o"] + PH_OFFSET, map_off(p["off"]))
        if p["type"] == PT_LOAD:
            struct.pack_into("<Q", out, p["o"] + PH_ALIGN, page)

    # ... and the section headers, whose offsets moved with the data.  Runtime
    # never reads these, but leaving them stale would make the artifact
    # misleading to anyone inspecting it.
    new_e_shoff = map_off(e.e_shoff)
    for sh in e.sh():
        # The headers themselves moved, so write at their new location.
        struct.pack_into("<Q", out, map_off(sh["o"]) + 0x18, map_off(sh["off"]))
    struct.pack_into("<Q", out, 0x28, new_e_shoff)
    print("  e_shoff %#x -> %#x" % (e.e_shoff, new_e_shoff))

    return bytes(out), len(loads)


def check_portable(e):
    """Assert every PT_LOAD is valid for each page size Linux actually uses."""
    bad = 0
    for p in (p for p in e.ph() if p["type"] == PT_LOAD):
        for page in PAGE_SIZES:
            if (p["vaddr"] - p["off"]) % page:
                bad += 1
            if p["align"] > 1 and p["align"] % page:
                bad += 1
    if bad:
        print("  FAIL: %d PT_LOAD invariants do not hold" % bad)
    else:
        print("  PT_LOAD congruent and aligned for 4 KiB, 16 KiB and 64 KiB pages")
    return bad == 0


def _page(p, page):
    """The first page holding any of segment `p`, and the last it touches."""
    lo = p["vaddr"] & ~(page - 1)
    hi = (p["vaddr"] + p["memsz"] - 1) & ~(page - 1)
    return lo, hi


def _shared_page(a, b, page):
    """True when segments `a` (lower vaddr) and `b` overlap a single page."""
    if b["vaddr"] < a["vaddr"] + a["memsz"] and a["vaddr"] < b["vaddr"] + b["memsz"]:
        return True                        # overlapping in vaddr
    return _page(b, page)[0] <= _page(a, page)[1]


def fix_segment_protections(b, e):
    """Fix 3: keep the protections a shared page needs, and unbreak RELRO.

    glibc maps PT_LOADs in program-header order and a later mapping wins for any
    page it shares with an earlier one.  A segment that shares a page with an
    earlier, differently-protected segment therefore has to carry the union of
    the two, or the earlier one's permissions are lost for that page — which is
    what makes the vendor's `.plt` non-executable.  Returns (n_flag_changes,
    n_relro_emptied)."""
    loads = sorted((p for p in e.ph() if p["type"] == PT_LOAD),
                   key=lambda p: p["vaddr"])
    flags_changed = 0

    for i, cur in enumerate(loads):
        for prev in loads[:i]:
            if not any(_shared_page(prev, cur, page) for page in PAGE_SIZES):
                continue
            want = cur["flags"] | prev["flags"]
            if want != cur["flags"]:
                print("  PT_LOAD vaddr %#x shares a page with vaddr %#x: "
                      "flags %#x -> %#x" % (cur["vaddr"], prev["vaddr"],
                                            cur["flags"], want))
                struct.pack_into("<I", b, cur["o"] + PH_FLAGS, want)
                cur["flags"] = want
                flags_changed += 1

    # _dl_protect_relro() mprotects [ALIGN_DOWN(start), ALIGN_DOWN(end)) to
    # PROT_READ.  If any of those pages holds executable bytes it destroys the
    # mapping we just fixed, so the range has to go.
    exe = [(p["vaddr"], p["vaddr"] + p["memsz"])
           for p in loads if p["flags"] & PF_X]
    relro_emptied = 0

    for r in (p for p in e.ph() if p["type"] == PT_GNU_RELRO):
        if not r["memsz"]:
            continue
        for page in PAGE_SIZES:
            lo = r["vaddr"] & ~(page - 1)
            hi = (r["vaddr"] + r["memsz"]) & ~(page - 1)
            if hi <= lo:
                continue
            hit = [s for s, t in exe if s < hi and lo < t]
            if hit:
                print("  PT_GNU_RELRO [%#x, %#x) overlaps an executable page at "
                      "page size %#x -> emptied" % (r["vaddr"],
                                                    r["vaddr"] + r["memsz"],
                                                    page))
                struct.pack_into("<Q", b, r["o"] + 0x20, 0)   # p_filesz
                struct.pack_into("<Q", b, r["o"] + 0x28, 0)   # p_memsz
                relro_emptied += 1
                break

    if not flags_changed and not relro_emptied:
        print("  segments are page-disjoint at 4/16/64 KiB — nothing to widen")
    return flags_changed, relro_emptied


def check_protections(e):
    """Assert a shared page never loses a protection it needs."""
    loads = sorted((p for p in e.ph() if p["type"] == PT_LOAD),
                   key=lambda p: p["vaddr"])
    bad = 0
    for i, cur in enumerate(loads):
        for prev in loads[:i]:
            if not any(_shared_page(prev, cur, page) for page in PAGE_SIZES):
                continue
            if (prev["flags"] & ~cur["flags"]) & (PF_X | PF_W):
                print("  FAIL: vaddr %#x would lose %#x on a shared page"
                      % (cur["vaddr"], prev["flags"] & ~cur["flags"]))
                bad += 1

    exe = [(p["vaddr"], p["vaddr"] + p["memsz"])
           for p in loads if p["flags"] & PF_X]
    for r in (p for p in e.ph() if p["type"] == PT_GNU_RELRO):
        if not r["memsz"]:
            continue
        for page in PAGE_SIZES:
            lo = r["vaddr"] & ~(page - 1)
            hi = (r["vaddr"] + r["memsz"]) & ~(page - 1)
            if hi > lo and any(s < hi and lo < t for s, t in exe):
                print("  FAIL: RELRO would PROT_READ an executable page at %#x"
                      % page)
                bad += 1

    if bad:
        print("  FAIL: %d shared-page protection invariants do not hold" % bad)
    else:
        print("  every shared page keeps its execute/write permission")
    return bad == 0


def fix_bind_now(b, e):
    """Fix 4: clear BIND_NOW, so imported-but-unused symbols need not resolve.

    Android links with `-Wl,-z,now` (full RELRO), which binds every imported
    symbol at load time.  The shim in this directory is built on the opposite
    assumption -- its documented contract is that "harnesses dlopen with
    RTLD_LAZY, so a symbol that is imported but never called need not resolve".
    Against a NOW library that contract cannot hold at all, and glibc refuses
    the dlopen before any code runs:

        undefined symbol: gettimeofday, version LIBC

    The MNN libraries need this: libMNN.so, libMNN_Express.so, libmnnmodel.so
    and the APK's libc++_shared.so are all NOW, and between them import ~137
    libc symbols the shim does not define (the whole locale and dirent surface,
    most of pthread, a dozen maths functions) -- none of which the inference
    path touches.

    Clearing the flag changes only *when* a symbol is resolved, never what the
    library computes.  A symbol that is genuinely needed still resolves, and if
    one were missing the harness would fail loudly on the first call rather than
    silently producing a wrong number.
    """
    DT_FLAGS, DT_FLAGS_1 = 0x1E, 0x6FFFFFFB
    DF_BIND_NOW, DF_1_NOW = 0x8, 0x1

    dyn = [s for s in e.sh() if s["type"] == 6]  # SHT_DYNAMIC
    if not dyn:
        print("  no .dynamic section; nothing to clear")
        return

    d = dyn[0]
    cleared = []
    for o in range(d["off"], d["off"] + d["size"], 16):
        tag, val = struct.unpack_from("<qQ", b, o)
        if tag == 0:  # DT_NULL
            break
        if tag == DT_FLAGS and val & DF_BIND_NOW:
            struct.pack_into("<Q", b, o + 8, val & ~DF_BIND_NOW)
            cleared.append("DF_BIND_NOW")
        elif tag == DT_FLAGS_1 and val & DF_1_NOW:
            struct.pack_into("<Q", b, o + 8, val & ~DF_1_NOW)
            cleared.append("DF_1_NOW")

    print("  cleared %s" % ", ".join(cleared) if cleared
          else "  already lazy (no BIND_NOW flag)")


def fix_crowded_pages(b, e, page):
    """Fix 5: merge PT_LOAD segments that crowd a single page.

    glibc rejects a program header layout in which **three or more** PT_LOAD
    segments share one memory page:

        libmnnmodel.so: ELF load command address/offset not page-aligned

    That message is misleading -- the segments' `p_vaddr` and `p_offset` are
    perfectly congruent -- but the check that fails lives in the same place.
    `libmnnmodel.so` is the shape that trips it: its three PT_LOADs sit at
    vaddr 0x0, 0xdb70 and 0xf120, so at a 16 KiB or 64 KiB page size all three
    land in page 0 (the last two also share a page at 4 KiB).  Its siblings are
    fine: libMNN.so's segments overlap only pairwise.

    This is established by experiment, not from glibc's source.  Measured on
    this host (16 KiB pages):

      * originals, as shipped           -> all four rejected
      * after fix 2 (offsets re-laid)   -> libMNN / libMNN_Express / libc++_shared
                                           load; libmnnmodel.so still rejected
      * libmnnmodel.so, LOAD1 truncated
        to the page boundary            -> loads (no page is shared three ways)
      * libmnnmodel.so, L1+L2 merged    -> still rejected
      * libmnnmodel.so, L2+L3 merged    -> still rejected
      * libmnnmodel.so, all three merged-> loads

    So the remedy is to merge the whole connected group: segments are joined
    when they share a page, and any component containing a crowded page is
    collapsed into a single PT_LOAD spanning all of it.  The merged segment
    takes the union of the members' flags, so it becomes R|W|X -- the same
    widening fix 3 already applies to segments that share a page, and with the
    same justification: this is a loader-compatibility change for a prepared
    copy whose only job is to produce ground truth for a differential harness.

    `p_vaddr` is never moved, so no relocation, symbol or `.dynamic` pointer
    has to be rewritten.  Libraries whose pages are shared only pairwise are
    left exactly as they were.
    """
    loads = [p for p in e.ph() if p["type"] == PT_LOAD]
    if len(loads) < 3:
        return

    def span(p):
        lo = p["vaddr"] & ~(page - 1)
        hi = (p["vaddr"] + p["memsz"] + page - 1) & ~(page - 1)
        return lo, hi

    # Which segments touch which page?
    by_page = {}
    for i, p in enumerate(loads):
        lo, hi = span(p)
        for pg in range(lo, hi, page):
            by_page.setdefault(pg, []).append(i)

    crowded = {pg for pg, who in by_page.items() if len(who) >= 3}
    if not crowded:
        print("  no page carries three PT_LOADs; layout left alone")
        return

    # Union-find over segments that share a page.
    parent = list(range(len(loads)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, c):
        ra, rc = find(a), find(c)
        if ra != rc:
            parent[max(ra, rc)] = min(ra, rc)

    for who in by_page.values():
        for other in who[1:]:
            union(who[0], other)

    # Merge every component that contains a crowded page.
    groups = {}
    for pg in crowded:
        for i in by_page[pg]:
            groups.setdefault(find(i), []).append(i)

    merged = 0
    for members in groups.values():
        members = sorted(set(members))
        if len(members) < 2:
            continue
        ps = [loads[i] for i in members]
        lo_off = min(p["off"] for p in ps)
        hi_off = max(p["off"] + p["filesz"] for p in ps)
        lo_va = min(p["vaddr"] for p in ps)
        hi_va = max(p["vaddr"] + p["memsz"] for p in ps)
        flags = 0
        for p in ps:
            flags |= p["flags"]

        head = ps[0]["o"]
        struct.pack_into("<IIQQQQQQ", b, head, PT_LOAD, flags, lo_off, lo_va,
                         lo_va, hi_off - lo_off, hi_va - lo_va, page)
        for p in ps[1:]:
            struct.pack_into("<I", b, p["o"], 0)  # PT_NULL

        print("  merged %d PT_LOADs (vaddr 0x%x..0x%x) into one, flags 0x%x"
              % (len(ps), lo_va, hi_va, flags))
        merged += 1

    if merged == 0:
        print("  no page carries three PT_LOADs; layout left alone")


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

    # A fixed 64 KiB, not the host's page size: the artifact must be the same
    # wherever it is produced, and valid on a host with a smaller page.
    page = max(os.sysconf("SC_PAGE_SIZE"), 0x10000)

    print("== fix 1: NULL init/fini array entries ==")
    fix_init_arrays(b, Elf(b))

    print("== fix 2: PT_LOAD alignment, target %d bytes ==" % page)
    b, _ = fix_page_alignment(b, Elf(b), page)
    b = bytearray(b)

    print("== fix 3: segment protections on shared pages ==")
    fix_segment_protections(b, Elf(b))

    print("== fix 4: eager binding ==")
    fix_bind_now(b, Elf(b))

    print("== fix 5: PT_LOAD segments crowding one page ==")
    fix_crowded_pages(b, Elf(b), page)

    print("== checking portability ==")
    if not check_portable(Elf(b)) or not check_protections(Elf(b)):
        sys.stderr.write("prepared library is not page-size portable\n")
        return 1

    with open(dst, "wb") as f:
        f.write(bytes(b))
    print("  wrote %s (%d bytes)" % (dst, len(b)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
