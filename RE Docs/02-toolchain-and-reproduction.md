# 02 — Toolchain, Workspace Layout and Reproduction

Everything in this document set can be regenerated from the two original artifacts.
This file is the recipe. **All paths are relative to the thermal-camera folder.**

```
Mechanic-Iscout-Thermal-Camera-linux-Port/                <- repository root
├── RE Docs/                                               <- THIS DOCUMENT SET
├── linux-port/                                            <- the Linux port (see 08)
├── RE Workspace/                                          <- generated, large, disposable
│   ├── apk/                  extracted APK contents (libs, assets, dex)
│   ├── jadx/                 jadx decompilation (sources/ + resources/)
│   ├── exe/                  installer carving + extracted NSIS payload
│   │   ├── nsis_blocks/
│   │   │   ├── blk000..blk206.bin          every NSIS data block
│   │   │   ├── _nsis_header.bin            decompressed NSIS header
│   │   │   ├── _nsis_header_strings.txt    581-entry UTF-16LE string table
│   │   │   ├── INVENTORY.md                block / size / kind / verified identity
│   │   │   ├── manifest.txt walk*.log      extraction records
│   │   │   └── walk2.py block_file_map.txt exploratory walker + its output
│   │   ├── boot1/ boot3/     WiX Burn bootstrapper tree (from blk203's 7z SFX)
│   │   └── *.cab             carved bootstrapper CABs (prerequisites only)
│   ├── ghidra/
│   │   ├── proj/             Ghidra project (ThermalCam)
│   │   ├── out/*.so.c        decompiled C for each native library
│   │   └── scripts/ExportDecompiled.java
│   ├── tools/
│   │   ├── nsis_extract.py   reusable NSIS extractor (written here)
│   │   ├── dotnet_id.py      authoritative .NET assembly identification
│   │   └── pe_ident.py       best-effort native PE identification
│   └── logs/                 tool logs
└── iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/
    ├── MechanicTi.apk
    └── iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe
```

> `RE Workspace/` is disposable. It is ~583 MB. Nothing in it is a primary source —
> it can always be rebuilt with the commands below. `.gitignore` excludes it, apart
> from the scripts under `tools/` and `ghidra/`, which are tracked because this
> document cites them as the reproduction pipeline.
>
> **Exception:** `_nsis_header_strings.txt` and `INVENTORY.md` encode recovered knowledge that is
> cheap to lose and annoying to redo. If you delete the workspace, keep those two (or accept
> re-running the extraction).

---

## 2.1 Tool inventory used

| Tool | Version | Notes |
|------|---------|-------|
| jadx | 1.5.6 (flatpak `com.github.skylot.jadx`) | invoked as `flatpak run --command=jadx com.github.skylot.jadx` |
| Ghidra | 12.1.2 PUBLIC | headless at `/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless` |
| OpenJDK | 21 (Corretto) | required by Ghidra |
| 7-Zip | 26.02 | **fails on the NSIS installer — see §2.4** |
| cabextract | — | for the bootstrapper CABs |
| binwalk | — | signature discovery only |
| python3 | 3.x | all custom parsing (`lzma`, `struct`) |
| readelf / nm / objdump | binutils | symbol and section inspection |

**Not available / not needed:** `wine`, `aapt`, `aapt2`, `apkanalyzer`, `innoextract`.

---

## 2.2 Android APK

```bash
# Run from the repository root (paths below are relative to it).
WS="RE Workspace"

# Full listing
unzip -l "iScout .../MechanicTi.apk" | head -60

# Native libs + assets + dex  (68 MB)
unzip -o -q "iScout .../MechanicTi.apk" "lib/arm64-v8a/*" "assets/*" "classes*.dex" -d "$WS/apk"

# Decompile Java (2 dex, 3658 classes, ~4 min, 204 MB output)
flatpak run --command=jadx com.github.skylot.jadx \
    -d "$WS/jadx" --show-bad-code -j 8 "iScout .../MechanicTi.apk"
```

Useful jadx outputs afterwards:

| Path | What |
|------|------|
| `$WS/jadx/resources/AndroidManifest.xml` | decoded manifest (package, permissions, activities) |
| `$WS/jadx/resources/res/xml/device_filter.xml` | **USB VID/PID filter** |
| `$WS/jadx/sources/com/dyt/wcc/` | the vendor application |
| `$WS/jadx/sources/com/serenegiant/usb/UVCCamera.java` | the JNI surface for the camera |

---

## 2.3 Native library analysis (Ghidra headless)

The export script is at `RE Workspace/ghidra/scripts/ExportDecompiled.java`.
It decompiles **every** function in the current program and writes C to a file.

```bash
GH=/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless
BASE="$PWD/RE Workspace"
LIBS="$BASE/apk/lib/arm64-v8a"

"$GH" "$BASE/ghidra/proj" ThermalCam \
      -import "$LIBS/libthermometry.so" \
      -scriptPath "$BASE/ghidra/scripts" \
      -postScript ExportDecompiled.java "$BASE/ghidra/out/libthermometry.so.c"
```

For a library already imported, replace `-import <file>` with
`-process <program name> -noanalysis` (much faster).

**Gotcha:** Ghidra's Java script compiler is strict. An `import` of a class that does not
exist fails the whole script with the misleading error
`ClassNotFoundException: ExportDecompiled not found`. Check for `error: cannot find symbol`
earlier in the log.

Libraries decompiled this session (ARM64, `AARCH64:LE:64:v8A`, image base `0x100000`):

| Library | Output size | Notes |
|---------|-------------|-------|
| `libthermometry.so` | 36 KB | tiny, fully understood |
| `libuvc.so` | 496 KB | modified libuvc; 359 functions |
| `libDYTJpegAes.so` | 1.26 MB | DYT image container + AES |
| `libUVCCamera.so` | 5.46 MB | JNI layer; bulk is bundled libyuv |

**VA → file offset mapping.** Image base is `0x100000`, so for most of the file:

```python
file_offset = VA - 0x100000
```

This was verified against a known `.rodata` string: the literal
`"D:/DYTPIRCamera/UvcConnect/src/main/jni/UVCCamera/UVCPreviewIR.cpp"` sits at file offset
`0x13ce04`, implying VA `0x23ce04`. **[V]**

> **Caution:** an earlier note in this analysis suggested an *additional* `-0x10000` adjustment for
> `VA >= 0x112d48`. That is **wrong** for the `.rodata` region and produced garbage when reading
> the AES tag strings. Use the plain `VA - 0x100000` and sanity-check against a known string
> before trusting any extracted constant.

Read constants directly:

```python
b = open("apk/lib/arm64-v8a/libUVCCamera.so","rb").read()
def dat(va, n): return b[va-0x100000 : va-0x100000+n]
```

This is how the opcode table in `04-usb-protocol.md` §4.2 was recovered.

`libUVCCamera.so` exports **no** `Java_*` symbols — JNI is bound dynamically in
`JNI_OnLoad` (`@ 0x5b8b0`). **[V]** Read the `JNINativeMethod` table there to map Java
method names to C functions.

### Reading constants out of an ARM64 `.so`

Ghidra addresses are `ELF vaddr + 0x100000`. For this library the mapping to file offset is:

```python
def foff(ghidra_va):
    v = ghidra_va - 0x100000
    return v - 0x10000 if v >= 0x12d48 else v   # second PT_LOAD is offset by 0x10000
```

Check `readelf -lW <lib>` to confirm the PT_LOAD offsets before trusting this.

---

## 2.4 Windows installer — the NSIS problem and its solution

### Why the obvious approaches fail

`file` reports the installer as *"Nullsoft Installer self-extracting archive"*, but:

```bash
7z l "iScout ... v3.0.6.exe"        # ERROR: Cannot open the file as archive
7z l -tNSIS "..."                   # ERROR: Is not archive
```

This is **not** a corrupt file. The installer is a genuine NSIS 3.07
(`Nullsoft Install System v22-Nov-2021.cvs`) that embeds:

* a Visual Studio bootstrapper (`.NET 4.6.1`, VC++ 2008/2010/2012/2013/2015) — **[V]**
* the actual application payload — **[V]**

7-Zip's NSIS handler searches backwards from EOF for the `0xDEADBEEF` + `"NullsoftInst"`
signature and finds a **coincidental occurrence at `0x118c6e54` inside compressed payload
data**, then gives up when that candidate fails to parse. Zeroing that occurrence does not
help, so 7-Zip 26.02 simply cannot read this file. **[V]** (tested both ways)

### The working extraction

Two independent implementations were written this session and cross-checked byte-for-byte:

* `RE Workspace/tools/nsis_extract.py` — **reusable, documented, recommended**
* `RE Workspace/exe/nsis_blocks/walk2.py` — the exploratory version

```bash
python3 "RE Workspace/tools/nsis_extract.py" \
        "iScout ... v3.0.6.exe" \
        "RE Workspace/exe/nsis_blocks" \
        --max-size 4000000      # skip writing multi-MB prerequisite blobs
```

Result: **207 blocks** extracted, `manifest.txt` + `blk000..blk206`.

### Format notes recovered (all **[V]**)

* FirstHeader at **`0x11600`** (512-byte aligned, and **exactly** where the overlay begins: the
  last section `.rsrc` has `rawptr=0x8a00`, `rawsz=0x8c00`, so its raw end is `0x11600`).
  `flags=0x50`, `length_of_header=65464`.
  > The published `FH_FLAGS_MASK = 0x0F` is an NSIS 2.x constraint and does **not** hold
  > here — do not use it as a validity filter.
  >
  > Verify with:
  > ```bash
  > python3 -c "
  > import struct,sys
  > d=open(sys.argv[1],'rb').read(); e=struct.unpack_from('<I',d,0x3c)[0]
  > n=struct.unpack_from('<H',d,e+6)[0]; o=struct.unpack_from('<H',d,e+20)[0]
  > s=e+24+o
  > print('overlay = %#x' % max(struct.unpack_from('<I',d,s+i*40+20)[0]
  >                            + struct.unpack_from('<I',d,s+i*40+16)[0] for i in range(n)))" \
  >   "...v3.0.6.exe"
  > # -> overlay = 0x11600
  > ```
* **The header is a Unicode build.** Its strings are **UTF-16LE**, so a plain `strings` pass
  returns nothing usable. Decode as UTF-16LE — see `07-windows-app-and-installer.md` §1.1.
  The full 581-entry table is saved at `exe/nsis_blocks/_nsis_header_strings.txt`.
* 8 zero bytes sit between the FirstHeader and the compressed header block; the block
  header starts at `firstheader + 36`.
* Header block: `u32 size` (= 10166, **includes** the 5-byte props), `u32 marker`
  (`0x80000000`), `u8 lzma_props` (`0x5d`), `u32 dict_size` (`0x00800000`), then the raw
  LZMA1 stream. Decompresses to exactly `length_of_header` = 65464 bytes.
* Data blocks start at `0x13DE2`. Each block:
  * `u32 size`, `u32 marker`
  * `marker == 0` → **stored**, `size` raw bytes follow
  * `marker == 0x80000000` → **LZMA**, 5-byte props/dict then `size - 5` bytes
  * advance = `8 + size`
* Blocks are **not** solid — every block carries its own props byte.

### Bootstrapper CABs (VC++ prerequisites)

Carved by offset (found with `binwalk`), then `cabextract`:

```bash
python3 -c "
d=open('...v3.0.6.exe','rb').read()
for off,size,name in [(0xC606D93,5625404,'cab1'),(0xCB6FAF3,4077389,'cab2'),(0xCF5FB03,4947186,'cab3')]:
    open(name+'.cab','wb').write(d[off:off+size])"
cabextract -d boot1 cab1.cab
```

These turn out to be **VC++ 2010 x64 and VC++ 2008 redistributable bootstrappers**
(`SetupEngine.dll`, `SetupUi.dll`, `sqmapi.dll`, `ParameterInfo.xml`) — i.e. Microsoft
prerequisites, not the application. **[V]**

A 7-Zip archive at `0xD865A29` (67 MB) sits inside NSIS block **`blk203`** (67,681,000 bytes,
stored — i.e. a 7-Zip **SFX**: a PE stub followed by the archive). Extracted, it yields the
**WiX Burn bootstrapper** (`Setup.exe`, `SetupEngine.dll`, `SetupUi.dll`, `ParameterInfo.xml`,
localized resources, `vc_red.cab`/`vc_red.msi`) plus the .NET Framework redistributable CABs.
All prerequisites. **[V]**

> **Note:** the `.cab` files carved by offset above and the `boot1`/`boot3` trees belong to this
> bootstrapper, **not** to the application. The application payload is the NSIS blocks
> (`blk000 … blk202`) — see `07-windows-app-and-installer.md` §1 and §3.

### Block identification tools

Three tools, in increasing order of authority. Prefer the third.

| Tool | Method | Authority |
|---|---|---|
| magic sniff | first 4 bytes | weak |
| `RE Workspace/tools/pe_ident.py` | `VS_VERSIONINFO` resources | weak — most vendor DLLs carry none |
| **`RE Workspace/tools/dotnet_id.py`** | **CLR metadata `Module` table, row 0** | **authoritative for .NET assemblies** |

```bash
cd "../RE Workspace"
python3 tools/dotnet_id.py exe/nsis_blocks    # 45 verified .NET assembly identities
python3 tools/pe_ident.py  exe/nsis_blocks    # best-effort native DLL names
```

> **Do not align the NSIS string table order with block order.** The string table is indexed by
> *first use*, not by `File` order, so the alignment drifts after the first few dozen entries.
> A naive index alignment was tested and produced **53 false attributions out of 186**. Use the
> CLR Module name instead. **[V]**

The consolidated result is `exe/nsis_blocks/INVENTORY.md`.

---

## 2.5 Reproducing the thermometry constants

```python
import struct
d = open('libthermometry.so','rb').read()
def f32(va): return struct.unpack('<f', d[va-0x100000:va-0x100000+4])[0]
def f64(va): return struct.unpack('<d', d[va-0x100000:va-0x100000+8])[0]
```

See `05-thermometry-algorithm.md` §5.6 for the full address→value table.
**Watch out:** some constants are `double` at 8-byte-aligned addresses while neighbouring
4-byte reads look like separate (nonsense) `float` values. If a constant is used in a
`double` expression in the decompilation, read 8 bytes.

---

## 2.6 Verification discipline

Every extraction in this workspace was cross-checked at least once:

* NSIS blocks: two independent walkers, `cmp`-identical for sampled blocks.
* Palettes: size 768 = 256 × RGB24, and the Android equivalents match in format.
* PE identification: module name read from the .NET metadata `Module` table / PE export
  directory — not guessed from strings.
* Thermometry: constants read directly from the binary at the addresses Ghidra referenced.

When extending this work, keep the same rule: **if it is not reproduced from bytes, tag it
`[I]` or `[?]`.**
