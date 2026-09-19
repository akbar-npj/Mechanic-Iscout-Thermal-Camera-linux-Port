# 07 — Windows Application and Installer

The Windows deliverable is a single 294 MB self-extracting installer. This document covers how it
is packaged, how to extract it, and what is inside. Tags follow `README.md` §3.

---

## 1. Packaging chain — solved

```
iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe     294,422,047 bytes (0x118C861F)
│
├─ PE32 stub                                    0x000000 … 0x011600
│    machine 0x14c (i386), 5 sections, magic 0x10b
│    .text .rdata .data .ndata .rsrc
│    end of last section = 0x11600  ← overlay begins exactly here
│
└─ NSIS 3.07 payload                            0x011600 … end
     ├─ FirstHeader @ 0x11600   flags=0x50   decompressed header = 65,464 B
     ├─ compressed header block @ 0x11624   10,166 B → 65,449 B (LZMA1)
     ├─ data blocks 0…206                   0x013DE2 … 0x118C8617
     │    ├─ blk000 … blk202   the application payload
     │    ├─ blk203           67,681,000 B stored = 7-Zip SFX
     │    │                     └─ payload_0xD865A29.7z (67,475,134 B)
     │    │                          └─ WiX Burn bootstrapper + VC++ redist CABs
     │    ├─ blk204           nsProcess.dll (4,096 B)
     │    ├─ blk205           23,618 B, non-PE
     │    └─ blk206           6,087 B stored = nested NSIS uninstaller
     └─ 8 bytes trailing padding
```

Verification commands:

```bash
EXE="../iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe"

# overlay begins exactly at the FirstHeader
python3 -c "
import struct,sys
d=open(sys.argv[1],'rb').read(); e=struct.unpack_from('<I',d,0x3c)[0]
n=struct.unpack_from('<H',d,e+6)[0]; o=struct.unpack_from('<H',d,e+20)[0]
s=e+24+o; end=max(struct.unpack_from('<I',d,s+i*40+20)[0]+struct.unpack_from('<I',d,s+i*40+16)[0] for i in range(n))
print('overlay = %#x' % end)" "$EXE"

# embedded 7z
python3 -c "
d=open('$EXE','rb').read(); i=d.find(b'7z\xbc\xaf\x27\x1c'); print('7z @ %#x' % i)"
# -> 7z @ 0xd865a29
```

**[V]** The WiX Burn / `vc_red.cab` / `vc_red.msi` content is a **red herring** for porting
purposes — it is Microsoft VC++ and .NET redistributable bootstrapping, not the application.

### 1.1 NSIS header is a Unicode build

**[V]** Strings in the decompressed header are **UTF-16LE**, not ASCII. This is why a naive
`strings` pass on `_nsis_header.bin` returns nothing useful. Use:

```bash
python3 -c "
import re
d=open('exe/nsis_blocks/_nsis_header.bin','rb').read()
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){2,}', d):
    print('%#08x\t%s' % (m.start(), d[m.start():m.end()].decode('utf-16-le')))" \
  | head -40
```

The full 581-entry string table is saved at
`exe/nsis_blocks/_nsis_header_strings.txt`. **[V]**

---

## 2. Extraction procedure

The bundled tool `../RE Workspace/tools/nsis_extract.py` does the whole job:

```bash
cd "../RE Workspace"
python3 tools/nsis_extract.py \
  "../iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe" \
  exe/nsis_blocks
```

Outputs:

| File | Contents |
|---|---|
| `blk000.bin` … `blk206.bin` | every decompressed data block |
| `blkNNN.<ext>` | duplicate of any block whose magic was recognised |
| `_nsis_header.bin` | decompressed NSIS header |
| `_nsis_header_strings.txt` | UTF-16LE string table dump |
| `manifest.txt` | block index, file offset, compressed size, decompressed size, method |
| `INVENTORY.md` | block / size / kind / verified identity table |

See `02-toolchain-and-reproduction.md` for the format details and the two bugs that had to be
solved (the 8-byte gap after the FirstHeader, and stored-vs-LZMA blocks).

### 2.1 Identifying blocks

Three complementary tools are provided, in increasing order of authority:

| Tool | What it does | Authority |
|---|---|---|
| magic sniff | first 4 bytes → PE / PDF / WAV / PNG / BMP / PDB / INI | weak |
| `tools/pe_ident.py` | reads `VS_VERSIONINFO` resources | weak (many DLLs have none) |
| **`tools/dotnet_id.py`** | reads the **CLR metadata Module name** | **authoritative for .NET assemblies** |
| `INVENTORY.md` | combines all of the above | — |

```bash
python3 tools/dotnet_id.py exe/nsis_blocks    # 45 verified .NET assembly identities
python3 tools/pe_ident.py  exe/nsis_blocks    # best-effort native DLL names
```

**Do not** try to align the NSIS string table order with block order. The string table is indexed
by *first use*, not by `File` order, so the alignment drifts after the first few dozen entries.
Use the CLR Module name instead. **[V]** — this was tested; a naive index alignment produced 53
false attributions out of 186.

---

## 3. Verified payload inventory

**[V]** 45 blocks are .NET assemblies, identified by their internal CLR `Module` name. This is
authoritative — the name comes from inside the assembly, not from a guess.

### 3.1 .NET assemblies (verified by CLR Module name)

| block | size | module name |
|---|---|---|
| blk005 | 131,072 | `Accord.dll` |
| blk006 | 40,960 | `Accord.Video.dll` |
| blk007 | 120,832 | `Accord.Video.FFMPEG.dll` |
| blk008 | 44,544 | `AForge.Controls.dll` |
| blk009 | 17,920 | `AForge.dll` |
| blk010 | 262,656 | `AForge.Imaging.dll` |
| blk011 | 16,384 | `AForge.Imaging.Formats.dll` |
| blk012 | 68,096 | `AForge.Math.dll` |
| blk013 | 61,440 | `AForge.Video.DirectShow.dll` |
| blk014 | 20,992 | `AForge.Video.dll` |
| blk015 | 61,952 | `AForge.Video.FFMPEG.dll` |
| blk050 | 2,204,160 | `CA09B.exe` |
| blk064 | 2,227,712 | `CAAnalyzer.exe` |
| blk066 | 2,227,200 | `CA09D.exe` |
| blk075 | 2,942,976 | `CA30D.exe` |
| blk088–blk094 | — | `*.resources.dll` satellite assemblies |
| blk097 | 14,848 | `FTPClient.dll` |
| blk100 | 202,240 | `ICSharpCode.SharpZipLib.dll` |
| blk130 | 178,176 | `Microsoft.DirectX.DirectSound.dll` |
| blk131 | 223,232 | `Microsoft.DirectX.dll` |
| blk133 | 21,216 | `Microsoft.Win32.Primitives.dll` |
| blk136 | 1,015,808 | `msvcm80d.dll` |
| blk137 | 1,028,096 | `msvcp80d.dll` |
| blk138 | 1,171,456 | `msvcr80d.dll` |
| blk139 | 13,312 | `MyControlLibrary.dll` |
| blk140 | 98,616 | `netstandard.dll` |
| blk141 | 945,152 | `OpenCvSharp.dll` |
| blk145 | 344,064 | `SharpGL.dll` |
| blk146 | 154,624 | `SharpGL.SceneGraph.dll` |
| blk147 | 19,456 | `SharpGL.WinForms.dll` |
| blk160 | 115,856 | `System.Numerics.Vectors.dll` |
| blk161 | 21,696 | `System.Xml.XDocument.dll` |
| **blk166** | **407,040** | **`ThermalAnalysisSystem.exe`** ← main application |
| blk171 | 35,328 | `UpdateTool.exe` |
| blk180 | 295,424 | `ZedGraph.dll` |
| blk181–blk189 | — | more satellite resource assemblies |

Note `msvcm80d` / `msvcp80d` / `msvcr80d` are the **debug** CRT (VS2005) — see §4.2.

### 3.2 Native DLLs — the parts that matter for the port

**[V]** The RSDS debug records inside these DLLs give their **exact original build paths**, which
reveal the vendor's source tree. This is far more reliable than magic sniffing.

| block | DLL | original build path (from RSDS) |
|---|---|---|
| blk046 / blk060 / blk071 | `libirparse.dll` | `E:\SDK\libirparse\Release\Win32\dll\`, `D:\WORK\mini386 640\DVP\libirparse\…` |
| blk047 / blk061 / blk072 | `libirprocess.dll` | `E:\SDK\libirprocess\…`, `D:\WORK\mini386 640\DVP\…` |
| blk048 / blk062 / blk073 | `libirtemp.dll` | `E:\SDK\libirtemp\…`, `D:\WORK\ASIC_384_640_1280\mini384_640\…` |
| blk049 / blk063 / blk074 | `libiruvc.dll` | `E:\SDK\libiruvc\…`, `D:\WORK\ASIC_384_640_1280\mini384_640\…` |
| blk070 | `libircmd.dll` | `D:\WORK\ASIC_384_640_1280\多镜头\libircmd\Release\Win32\dll\` |
| blk081 | `DcontrolDll.dll` | `C:\Users\Administrator\Desktop\640dll_集成平台\DcontrolDll\Release\` |
| blk082 | **OpenCV core 3.4.15** | `E:\OpenCV\opencv-3.4.15\build\bin\Release\opencv_core3415.pdb` |
| blk096 | `ExternalDevicesG2.dll` | `D:\ObjectWorkspace\C++\ExternalDevicesG2\Release\` |
| blk101 | `jpegext.dll` | — |
| blk143 | `postproc-54.dll` | — |
| blk157 | `swresample-2.dll` | — |
| blk159 | `swscale-4.dll` | — |
| blk164 | `Temperature.dll` | — |
| blk169 | `Tiny1BDll.dll` | `E:\Mini640\Tiny1BDll\Release\` |
| blk170 | `Tiny1CDll.dll` | `C:\Users\Administrator\Desktop\tinyC源码（兼容S0）\Release\` |
| blk172 | `UVCController.dll` | `D:\ObjectWorkspace\C++\UVCController\Release\` |
| blk173 | `Videoext.dll` | — |
| blk179 | `XthermDll.dll` | `d:\Work\XthermSDK\XthermDll - 点扬\release\` |

Reproduce the RSDS dump:

```bash
cd "../RE Workspace"
python3 - <<'EOF'
import os, re, struct
def rva2off(b, va):
    e=struct.unpack_from('<I',b,0x3c)[0]
    nsec=struct.unpack_from('<H',b,e+6)[0]; optsz=struct.unpack_from('<H',b,e+20)[0]
    sec=e+24+optsz
    for i in range(nsec):
        o=sec+i*40
        vsz,vaddr,rsz,raddr=struct.unpack_from('<IIII',b,o+8)
        if vaddr<=va<vaddr+max(vsz,rsz): return raddr+(va-vaddr)
def rsds(b):
    e=struct.unpack_from('<I',b,0x3c)[0]; opt=e+24
    magic=struct.unpack_from('<H',b,opt)[0]; dd=opt+(96 if magic==0x10b else 112)
    rva,sz=struct.unpack_from('<II',b,dd+6*8)
    if not rva: return []
    o=rva2off(b,rva); out=[]
    for i in range(sz//28):
        ent=o+i*28
        if struct.unpack_from('<I',b,ent+12)[0]!=2: continue
        dp=struct.unpack_from('<I',b,ent+24)[0]
        if b[dp:dp+4]==b'RSDS':
            out.append(b[dp+24:b.index(b'\0',dp+24)].decode('latin1'))
    return out
for fn in sorted(os.listdir("exe/nsis_blocks")):
    m=re.match(r'blk(\d+)\.bin$',fn)
    if not m: continue
    b=open("exe/nsis_blocks/"+fn,'rb').read()
    if b.startswith(b'MZ'):
        for p in rsds(b): print("blk%03d %s" % (int(m.group(1)), p))
EOF
```

### 3.3 Which DLL does what — inferred roles

| DLL | Role | Basis |
|---|---|---|
| `libiruvc.dll` | USB/UVC transport for the IR camera | name; mirrors `libuvc.so` on Android |
| `libircmd.dll` | vendor command layer (the control-transfer opcodes) | name; mirrors `libUVCCamera.so` |
| `libirparse.dll` | frame/stream parsing | name |
| `libirprocess.dll` | image processing | name |
| `libirtemp.dll` | thermometry | name; mirrors `libthermometry.so` |
| `Temperature.dll` | thermometry, C++/CLI wrapper | name |
| `Dcore.dll` | core image pipeline | name |
| `DcontrolDll.dll` | device control | name |
| `UVCController.dll` | UVC device lifecycle | name |
| `jpegext.dll` | DYT container read/write | mirrors `libDYTJpegAes.so` |
| `Tiny1BDll.dll`, `Tiny1CDll.dll`, `XthermDll.dll` | support for other sensor modules | build paths (`Mini640`, `XthermSDK`) |
| `ExternalDevicesG2.dll` | external device integration | name |
| `Videoext.dll` | video helpers | name |

**These roles are [I], inferred from names and the Android equivalents.** Confirming them
requires disassembling `libirtemp.dll` (for calibration) — see
`09-open-questions-and-next-steps.md` §3.

### 3.4 Data files

| File | Block(s) | Notes |
|---|---|---|
| `lut_1.dat` … `lut_27.dat` | blk103 … blk129 | 27 × 768 B, 256 × RGB — see `06-asset-and-file-formats.md` §1 |
| `tau_H.bin`, `tau_L.bin` | blk162, blk163 | **7168 B each** = `uint16[56][64]` Q14 transmittance, no header — see `10-calibration-tables.md` §2 |
| `MILI6_{H,L}_{500,91}.bin` | blk134, blk135 | **7424 B each** = 256-byte header + `uint16[56][64]`; only two distinct blobs for four declared names — see `10-calibration-tables.md` §3 |
| `block_lut.dat` | **[?]** | referenced only by the .NET layer (`CA30D.exe`); not read by any native thermometry code |
| `hash.txt` | **[?]** | declared at the install root but absent from the payload — generated at install time |
| `01-Chime.wav` … `08-Bell.wav` | blk148 … blk155 | RIFF/WAV alarm tones |
| `form_setting*.dat` | blk026/027, blk051–054, blk068/069, blk076–080 | `[CONFIG]` INI files |
| `logo.png` | blk174 | UI branding |
| `modern-wizard.bmp` | blk003 | NSIS wizard bitmap |
| `ioSpecial.ini` | blk002 | NSIS InstallOptions page definition |
| `ThermalAnalysis.pdb`, `ThermalAnalysisSystem.pdb` | blk165, blk168 | **[I]** — the only two PDB blocks and the only two `.pdb` names in the file list, so the mapping is one-to-one; identity not byte-confirmed |
| `Tutorials*.pdf`, `*.pdf` | blk028–045, blk055–059, blk065, blk098, blk099 | documentation |

The calibration rows above were resolved on 2026-09-20 by disassembling the loaders rather than by
decoding the NSIS file table — `blk081` is `DcontrolDll.dll` (loads `MILI6_*.bin`) and `blk170` is
`Tiny1CDll.dll` (loads `tau_*.bin`). Both identities come from RSDS paths inside the blocks. **[V]**
See `10-calibration-tables.md` §6 for why the NSIS file-record table could not be decoded.

### 3.5 The complete file list

**[V]** 179 distinct filenames were recovered from the NSIS header string table. Saved at
`exe/nsis_blocks/_nsis_header_strings.txt`. The application-relevant subset:

**Native DLLs**
```
libirparse.dll  libirprocess.dll  libirtemp.dll  libiruvc.dll  libircmd.dll
Temperature.dll  Dcore.dll  Dimgproc.dll  DcontrolDll.dll  UVCController.dll
Videoext.dll  jpegext.dll  ExternalDevicesG2.dll  Tiny1BDll.dll  Tiny1CDll.dll
XthermDll.dll  OpenCvSharpExtern.dll  cvextern.dll  pthreadVC2.dll
opencv_videoio_ffmpeg490.dll  opencv_videoio_ffmpeg490_64.dll
opencv_ffmpeg310.dll  opencv_ffmpeg310_64.dll
avcodec-53.dll  avcodec-57.dll  avdevice-53.dll  avdevice-57.dll
avfilter-2.dll  avfilter-6.dll  avformat-53.dll  avformat-57.dll
avutil-51.dll  avutil-55.dll  postproc-52.dll  postproc-54.dll
swresample-0.dll  swresample-2.dll  swscale-2.dll  swscale-4.dll
msvcm80d.dll  msvcp80d.dll  msvcr80d.dll
```

**Executables**
```
ThermalAnalysisSystem.exe   <- main application
UpdateTool.exe              <- auto-updater
CA09B.exe  CA09D.exe  CA30D.exe  CAAnalyzer.exe   <- OEM-branded variants
M1.exe  M2.exe              <- [?] purpose unknown
```

**Calibration / data**
```
tau_H.bin  tau_L.bin
MILI6_H_500.bin  MILI6_H_91.bin  MILI6_L_500.bin  MILI6_L_91.bin
block_lut.dat  hash.txt  form_setting*.dat  lut_1.dat … lut_27.dat
```

**Assemblies**
```
Accord.dll  Accord.Video.dll  Accord.Video.FFMPEG.dll
AForge.dll  AForge.Controls.dll  AForge.Imaging.dll  AForge.Imaging.Formats.dll
AForge.Math.dll  AForge.Video.dll  AForge.Video.DirectShow.dll  AForge.Video.FFMPEG.dll
OpenCvSharp.dll  SharpGL.dll  SharpGL.SceneGraph.dll  SharpGL.WinForms.dll
ZedGraph.dll  ICSharpCode.SharpZipLib.dll  FTPClient.dll  Kogel.Record.dll
MyControlLibrary.dll  Microsoft.DirectX.dll  Microsoft.DirectX.DirectSound.dll
netstandard.dll  System.Numerics.Vectors.dll  System.Xml.XDocument.dll
Microsoft.Win32.Primitives.dll
DytSpectrumOwl.resources.dll  Thermal Analysis System.resources.dll
CA09B.resources.dll  CA09D.resources.dll  CA30D.resources.dll
CAAnalyzer.resources.dll  CA_TA_10.resources.dll
ThermalAnalysisSystem.resources.dll  UpdateTool.resources.dll
```

**Redistributables (ignore for porting)**
```
vcredist_{x86,x64}_{2008,2010}.exe  vcredist_{2012,2013,2015}_{x86,x64}.exe
NDP461-KB3102436-x86-x64-AllOS-ENU.exe
```

---

## 4. Observations relevant to the port

### 4.1 The Windows build ships calibration the Android build does not

**[V]** `tau_*.bin`, `MILI6_*.bin` and `block_lut.dat` appear **only** in the Windows installer.
The Android APK has no equivalent. This suggests the Windows build performs per-unit calibration
in software, while the Android build either relies on values programmed into the camera or uses
a fixed factory table.

**Action for the port:** decode these files. They are the only calibration data available offline,
and they may be exactly what a Linux implementation needs for accurate absolute temperatures.
See `09-open-questions-and-next-steps.md` §3.

### 4.2 Debug CRT is shipped

**[V]** `msvcm80d.dll`, `msvcp80d.dll`, `msvcr80d.dll` are the **debug** variants of the VS2005
C runtime (`d` suffix). Shipping debug CRT is unusual in a release product and indicates at least
some components were built in Debug configuration — corroborated by `blk075`'s RSDS path ending
`\obj\x86\Debug\CA30D.pdb`.

### 4.3 Vendor source tree revealed by build paths

**[V]** From the RSDS paths, the vendor's layout is roughly:

```
E:\SDK\libir{parse,process,temp,uvc}\Release\Win32\dll\     <- the IR SDK
D:\WORK\ASIC_384_640_1280\mini384_640\libirtemp\...         <- newer sensor family
D:\WORK\mini386 640\DVP\libirparse\...                      <- older sensor family
E:\Mini640\Tiny1BDll\                                       <- Mini640 module
D:\Work\XthermSDK\XthermDll - 点扬\                          <- Xtherm module
D:\ObjectWorkspace\C++\{UVCController,ExternalDevicesG2}\   <- Windows host code
F:\CA系列集中\CA10平台\CATA10系列品牌集中\维修佬\...           <- the app tree
```

This confirms the SDK is a shared, multi-product library (multiple sensor families: 384/640/1280,
Mini384, Mini640, ASIC) and that this product (`CA_TA_10`) is one of many OEM builds.

### 4.4 `jpegext.dll` is the Windows twin of `libDYTJpegAes.so`

**[V]** Both are 14,848 bytes. **[I]** Almost certainly the same source compiled for two targets.
The DYT container format in `06-asset-and-file-formats.md` §2 therefore applies to both.

---

## 5. Do we need the Windows build at all?

For the **Linux port**, the Android build is the better primary source:

| | Android | Windows |
|---|---|---|
| Native code | ARM64, decompiled cleanly in this session | x86, not yet decompiled |
| Transport | `libuvc.so` — upstream libuvc, directly reusable on Linux | `libiruvc.dll` — WinUSB-based, not reusable |
| Thermometry | `libthermometry.so` — 13 KB, fully recovered | `libirtemp.dll` — equivalent |
| Calibration data | none | **present** |

**Conclusion [I]:** build the Linux client from the Android findings
(`04-usb-protocol.md`, `05-thermometry-algorithm.md`), and use the Windows build only as a source
of calibration tables and as a cross-check for the parts of the protocol that are still unclear.
