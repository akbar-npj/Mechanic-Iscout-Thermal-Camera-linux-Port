# 06 — Asset and File Formats

Everything in this document is derived from the extracted artifacts listed in
`02-toolchain-and-reproduction.md`. Tags follow `README.md` §3 (`[V]` verified, `[I]` inferred,
`[?]` unknown).

---

## 1. Colour palettes (LUTs)

### 1.1 Format — fully solved

**[V]** A palette file is exactly **768 bytes = 256 entries × 3 bytes**, one byte per channel,
stored **R, G, B** in that order.

Evidence — the Android loader `CreateBitmap.toByteArray()`:

```java
// jadx/sources/com/dyt/wcc/utils/CreateBitmap.java:25-28
for (int i3 = 0; i3 < 256; i3++) {
    int i4 = i3 * 3;
    iArr2[255 - i3] = iArr[i4 + 2] | (iArr[i4] << 16) | ViewCompat.MEASURED_STATE_MASK | (iArr[i4 + 1] << 8);
}
```

Android packs colours as `ARGB` ints, i.e. `A<<24 | R<<16 | G<<8 | B`. Therefore:

| file byte | meaning |
|---|---|
| `i4 + 0` | **R** (shifted into bits 16-23) |
| `i4 + 1` | **G** (shifted into bits 8-15) |
| `i4 + 2` | **B** (bits 0-7) |

So the file order is **R, G, B**. **[V]**

### 1.2 Index direction — solved

**[V]** The loader writes `iArr2[255 - i3] = palette[i3]`, i.e. the LUT is **reversed** into the
output bitmap. The bitmap is 1 px wide × 256 px tall, used as a vertical colour gradient.

Consequence for the Linux port: **palette index 0 is the coldest colour and index 255 is the
hottest**. Do not invert the ramp again.

Sanity check on `1.dat` (`apk/assets/1.dat`), sampled every 32 entries:

| index | R,G,B | interpretation |
|---|---|---|
| 0 | 0,0,0 | black (coldest) |
| 32 | 52,0,141 | deep purple |
| 64 | 141,0,157 | purple |
| 96 | 199,13,136 | magenta |
| 128 | 231,69,24 | red-orange |
| 160 | 245,120,0 | orange |
| 192 | 254,178,1 | amber |
| 224 | 255,227,44 | yellow |
| 255 | 255,255,245 | near-white (hottest) |

This is the classic "iron / rainbow" ramp. **[V]**

### 1.3 Palette inventory

| Build | Location | Count | Names |
|---|---|---|---|
| Android | `apk/assets/` | 7 | `1.dat` … `7.dat` |
| Windows | installer blocks 103-129 | 27 | `lut_1.dat` … `lut_27.dat` |

**[V]** `com.dyt.wcc.constans.DYConstants.paletteArrays` only lists `1.dat` … `6.dat`; `7.dat`
ships but is not referenced by that array.

**[V]** Palette selection is stored in SharedPreferences under `DYConstants.PALETTE_NUMBER`
(default `1`) and pushed to native code via
`AbstractUVCCameraHandler.PreparePalette(path, index, BuildConfig.CONFIGS_NAME)`
(`PreviewFragment.java:2100`).

**[V]** 6 of the 7 Android palettes are **byte-identical** to Windows palettes:

| Android | md5 | Windows equivalent |
|---|---|---|
| `1.dat` | `429de09b5afb…` | `lut_12.dat` (blk114) |
| `2.dat` | `88334c3f2bc5…` | `lut_22.dat` (blk124) |
| `3.dat` | `85ac61919da9…` | `lut_24.dat` (blk126) |
| `4.dat` | `4b41d71aa554…` | `lut_19.dat` (blk121) |
| `5.dat` | `fc55558e8169…` | `lut_18.dat` (blk120) |
| `6.dat` | `f78bd4201911…` | *(no match in this installer)* |
| `7.dat` | `c8f3c5385a39…` | `lut_20.dat` (blk122) |

Reproduce:

```bash
cd "../RE Workspace"
python3 - <<'EOF'
import hashlib
a = {i: hashlib.md5(open("apk/assets/%d.dat"%i,'rb').read()).hexdigest() for i in range(1,8)}
w = {b: hashlib.md5(open("exe/nsis_blocks/blk%03d.bin"%b,'rb').read()).hexdigest() for b in range(103,130)}
inv = {v:k for k,v in w.items()}
for i,h in a.items():
    print(i, "->", ("blk%03d"%inv[h]) if h in inv else "no match")
EOF
```

### 1.4 Palette names as used by the UI

**[V]** From `PreviewFragment.java:1805-1815`:

| variable | file | meaning |
|---|---|---|
| `tiehong` | `1.dat` | 铁红 — iron red |
| `caihong` | `2.dat` | 彩虹 — rainbow |
| `hongre` | `3.dat` | 红热 — red hot |
| `heire` | `4.dat` | 黑热 — black hot |
| `baire` | `5.dat` | 白热 — white hot |
| `lenglan` | `6.dat` | 冷蓝 — cool blue |

---

## 2. The DYT image container (`.jpg` / `libDYTJpegAes.so`)

**[V]** `libDYTJpegAes.so` is a `DT_NEEDED` dependency of `libUVCCamera.so`
(`readelf -d`), so it is loaded on every run. It is **not** called from Java — it is called from
the C++ capture path.

### 2.1 Container layout — solved

A DYT `.jpg` is an ordinary JPEG with a run of **APP2 (`FF E2`) segments spliced in immediately
after the APP0/APP1 header segments**. Verified from the writer `D_updateData` and reader
`D_jpegOpen` in `ghidra/out/libDYTJpegAes.so.c`.

```
[ FF D8 SOI ]
[ FF E0 APP0  ]              <- normal JFIF header
[ FF E1 APP1  ]*             <- any number, e.g. EXIF
+----------------------------------------------------------+
| FF E2 <len=hdrsz+2> <DYT header blob, hdrsz bytes>        |   <- inserted here
| FF E2 <len> <payload chunk 0>                             |
| FF E2 <len> <payload chunk 1>                             |
| ...  ceil((total-hdrsz)/0xFFFD) chunks ...                |
+----------------------------------------------------------+
[ FF C0 SOF0 … FF D9 EOI ]   <- the rest of the original JPEG, untouched
```

Key facts:

| Property | Value | Evidence |
|---|---|---|
| Insertion point | immediately after the last APP1 (`FF E1`) segment | `D_updateData` scan loop |
| Max chunk payload | `0xFFFD` (65533) bytes | `param_3 % 0xfffd`, `iVar3 < 0xfffe` |
| Full-chunk length field | written as `0xFFFF` | `__ptr_01[2]=0xff; __ptr_01[3]=0xff;` |
| Segment length field | **includes** the 2 length bytes | `len = payload + 2` |
| Header blob size | `u16` at **offset 0x06 of the header blob itself** | `*(ushort*)(param_2 + 6)` |

The header blob is **self-describing**: its own size is stored at `+0x06`, and the raw thermal
data begins immediately after it.

### 2.2 DYT header blob — field map

**[V]** Each getter is a fixed-offset `memcpy` out of the blob. Offsets are relative to the start
of the blob.

| Offset | Size | Getter | Contents |
|---|---|---|---|
| `0x000` | `0x18` (24) | `D_getFileHead` | file head; **`u16` at `+0x06` = blob size / raw-data offset** |
| `0x018` | `0x18` (24) | `D_getIrDataPar` | IR data parameters |
| `0x030` | `0x54` (84) | `D_getCameraInfo` | camera info |
| `0x084` | `0x64` (100) | `D_getSensorInfo` | sensor info |
| `0x0E8` | `0x58` (88) | `D_getLensInfo` | lens info |
| `0x140` | `0x54` (84) | `D_getCameraPar` | camera parameters |
| `0x194` | `0x2C` (44) | `D_getImageInfo` | image info |
| `0x1C0` | `0x6C` (108) | `D_getFuseInfo` | fusion info |
| `0x22C` | `0x30` (48) | `D_getGpsInfo` | GPS info |
| `0x25C` | `0x400` (1024) | `D_getTable` | per-file colour table |
| `0x65C` | `0x0C` (12) | `D_getShapeInfo` | 4 × `u8` counts: points, lines, rects, polygons |
| `0x668` | variable | `D_getShapePoints` / `Lines` / `Recs` / `Polygons` | annotation geometry |
| `u16@0x06` | variable | `D_getRawData` | **raw thermal frame data** |

Shape geometry is stored sequentially after `0x668`, each element a fixed stride:

| array | count byte | stride |
|---|---|---|
| points | `+0x65C` | `0x1C` (28) |
| lines | `+0x65D` | `0x68` (104) |
| rects | `+0x65E` | `0x34` (52) |
| polygons | `+0x65F` | `0x68` (104) |

The minimum header size is therefore `0x668` = 1640 bytes before any shape data.

### 2.3 API surface

**[V]** Exported functions in `libDYTJpegAes.so`:

```
T_openFile(char *path)                                  @ 0x129d20   truncate/create a file
D_updateData(path, hdr_blob, total_len)                 @ 0x129d7c   write container
D_saveData  (path, blob_pp, total_len)                  @ 0x12a6d4   write container
D_getDytFileLength(path, int *out)                      @ 0x12af48   payload size only
D_jpegOpen  (path, buf_pp, int *len)                    @ 0x12b464   read payload back
D_jpegClose()                                           @ 0x12ba3c
D_getFileHead / IrDataPar / CameraInfo / SensorInfo /
  LensInfo / CameraPar / ImageInfo / FuseInfo /
  GpsInfo / Table / ShapeInfo / ShapePoints /
  ShapeLines / ShapeRecs / ShapePolygons / RawData
D_updateFileHead / IrDataPar / CameraInfo / SensorInfo /
  LensInfo / CameraPar / ImageInfo / FuseInfo /
  GpsInfo / Table / Shape
```

JNI entry point found: `Java_com_dywcc_demojni_NativeUtils_JavaStaticCallD_1getDytFileLength`
— note the Java package is **`com.dywcc.demojni`**, which does **not** exist in this APK's dex.
This confirms the library is shared with other DYT products. **[V]**

### 2.4 Other strings in this library

| string | VA | meaning |
|---|---|---|
| `===jpegext===` | — | log tag |
| `DYTCQ10Q` | `0x116c77` | product code |
| `dyt0526c` | `0x116b0a` | version/date tag (2026-05-26?) |
| `dyt1101c` | `0x1175ac` | version/date tag (2025-11-01?) |
| `sn_length` | `0x1060d3` | serial-number length, next to `base64_decode` |
| `getTableEntrySize` | — | unwinder symbol only, not app code |

**[V]** `AES::AES()` (`_ZN3AESC2Ev` @ `0x126908`) copies two NUL-padded 17-byte strings into the
object at `+0x478` and `+0x489`:

```
+0x478 : "dyt1101c"  + 9 × 0x00        (16-byte value + terminator)
+0x489 : "dyt0526cdyt0526c" + 0x00     (16-byte value + terminator)
```

These are the AES key and IV for this class — see §3.2.

---

## 3. Configuration files

### 3.1 Windows `[CONFIG]` files (plain INI)

**[V]** The Windows installer ships several `[CONFIG]` INI files. Extracted blocks:
`blk026`, `blk027`, `blk051`–`blk054`, `blk068`, `blk069`, `blk076`–`blk080`.

Example — `blk026` (settings, plaintext):

```ini
[CONFIG]
Lut=0
HighWarn=False
HighWarnTemp=20.0
SoundIntervalSeconds=5
WarningSoundName=03-Falling.wav
TempUnit=1
ShowHeightTemp=True
ShowPointTemp=True
LanguageCode=zh
ImagePath=F:\CA\CA10\CATA10\CA_TA_10\ThermalAnalysisSystem\bin\x86\Release\Picture
VideoPath=F:\CA\CA10\CATA10\CA_TA_10\ThermalAnalysisSystem\bin\x86\Release\Video
Version=3.0.6
fileDownPath=JCWXLCA09B/JCWXLCA09B_3.0.3
fileUpdatePath=Version
AutoExamineUpdate=1
TaskPath=F:\CA\CA10\CATA10\CA_TA_10\ThermalAnalysisSystem\bin\x86\Release\Task
TempPicVal=50.0
FixedTempMax=80
TaskFrequency=5
FixedTempMin=0
```

**[V]** `blk027` is the same file with **base64-looking ciphertext** values:

```ini
[CONFIG]
Title=
Titleen=
Titlezh-Hant=
Mail=07U3UWZVCIgrRAFJu+mYMYSs7Wb2vOqX
Website=PEI3/goZW6d5JPmm/sZYlg==
Websiteen=PEI3/goZW6fmCVK4JoX8vCwNjQEtCRAy
Path=eeWPuDNh91JwQcRw8KCoZvjRVBF81mn9
Company=ETpKGF21GRYgVLuClkg/EuGQ8l18xyFoB2YCy1n1vV/PPAx8V2yBSA==
Companyen=BsOXzYnQxtptD2qJ/w/n7HGNB4MaegATQvaIblWHQ9AmNwvVDeftcy4xJqMyTJhT
```

Note these decode to **24, 16, 23, 24, 48, 48** bytes — not all block-aligned, so this is **not**
the same AES-CBC scheme as §3.2. **[I]**

### 3.2 Android `configs_maintenanceguy.txt` — a serial-number allow-list

This one is **fully traced and is functionally important**. **[V]**

The asset is:

```
yli6PhIDL6/rw8I2zJiiVQ==;O4CB46unY+T0DfC3e11NvA==;6G4s4lkzlHHP++idImyZCA==
```

Three base64 values, each decoding to exactly 16 bytes. `AssetCopyer.copyAllAssets()` copies it
to external storage (`AssetCopyer.java:34`), and `PreviewFragment.java:2100` passes its path to
native code via `PreparePalette(path, paletteIndex, "configs_maintenanceguy")`.

The native consumer is `UVCPreviewIR::do_preview` in `libUVCCamera.so`
(`ghidra/out/libUVCCamera.so.c:26718-26809`). Decompiled logic:

1. Read the file into memory.
2. `split(contents, ";")`.
3. For each element: construct `AES`, call `AES::DecryptionAES(element)`.
4. Take `substr(decrypted, 0, 8)`.
5. Compare those 8 bytes against the **camera's own serial number**, which was decrypted from the
   frame buffer into `this+0xb2c` by `UVCPreviewIR::DecryptSNE(...)`.
6. On any match → log `"verify sn success"` and set the verified flag `this+0xaf1 = 1`.

So this file is a **licence / authorisation allow-list of device serial numbers**, AES-encrypted,
separated by `;`. The comparison length is hard-coded to **8 bytes**.

### 3.3 The AES parameters — recovered

**[V]** Key and IV are hard-coded in `libDYTJpegAes.so` and copied into every `AES` instance by
`AES::AES()` (see §2.4). Decrypting the three config values with
**AES-128-CBC, key = IV = `"dyt1101c"` NUL-padded to 16 bytes** gives:

| ciphertext (base64) | plaintext (16 bytes) | first 8 = serial |
|---|---|---|
| `yli6PhIDL6/rw8I2zJiiVQ==` | `DYTEPK78lq\|8=:>k` | **`DYTEPK78`** |
| `O4CB46unY+T0DfC3e11NvA==` | `DYCRPK78lq\|8=:>k` | **`DYCRPK78`** |
| `6G4s4lkzlHHP++idImyZCA==` | `DYCRPK79lq\|8=:>k` | **`DYCRPK79`** |

Independent corroboration that the key is right:

* **Serial format matches.** The native libraries contain real serials `DYTCA09B`,
  `DYTCA10DJK010010` and `DYTCQ10Q` — all `DYT` + product code. The recovered `DYTEPK78` follows
  the same shape. **[V]**
* **Differential test.** For a wrong key, AES output is pseudorandom. With this key,
  `D(C₁) ⊕ D(C₂)` has **14 of 16 bytes zero** and `D(C₂) ⊕ D(C₃)` has **15 of 16 zero**, versus
  0–1 of 16 for control keys. This is exactly the expected signature of CBC with a shared IV:
  `D(C) = P ⊕ IV`, so near-identical plaintexts give near-identical `D(C)`. **[V]**

Reproduce:

```bash
cd "../RE Workspace"
python3 - <<'EOF'
import base64
from Crypto.Cipher import AES
K = b"dyt1101c" + b"\x00"*8
for v in ["yli6PhIDL6/rw8I2zJiiVQ==",
          "O4CB46unY+T0DfC3e11NvA==",
          "6G4s4lkzlHHP++idImyZCA=="]:
    p = AES.new(K, AES.MODE_CBC, K).decrypt(base64.b64decode(v))
    print(p[:8].decode(), "|", p)
EOF
```

**Caveat [I]:** the class stores *two* 16-byte values (`dyt1101c`, `dyt0526cdyt0526c`) and we
have not yet read the key-schedule code that says which is key and which is IV. The alternative
assignment (key = `dyt1101c…`, IV = `dyt0526cdyt0526c`) also yields plausible 8-byte serials —
`DYTDTI08`, `DYCSTI08`, `DYCSTI09`. Both candidates are listed in
`09-open-questions-and-next-steps.md` §5. This does **not** block the Linux port: we are writing
a client, not reproducing the vendor's licence check.

---

## 4. Calibration binaries

The Windows build ships calibration tables that the Android build does not. All are in the
installer block dump. **Decoded on 2026-09-20 — see `10-calibration-tables.md` for the full
format and the evidence.**

| File | Block(s) | Size | Status |
|---|---|---|---|
| `tau_H.bin` | blk162 | 7168 | **[V]** `uint16[56][64]` Q14 transmittance, no header |
| `tau_L.bin` | blk163 | 7168 | **[V]** same |
| `MILI6_{H,L}_{91,500}.bin` | blk134, blk135 | 7424 | **[V]** 256-byte header + `uint16[56][64]`; only two distinct blobs exist for the four declared names |
| `block_lut.dat` | — | — | **[?]** referenced only by the .NET layer; no native thermometry code reads it |
| `hash.txt` | — | — | **[?]** declared at the install root but absent from the payload |

Row = target-temperature band (248.15 K … 1623.15 K, 25 K steps, 56 entries).
Column = distance band (0.25 m … 50 m, 64 entries).
Value = **transmittance × 16384** — `Q14` fixed point, not IEEE floats. The scale factor `2⁻¹⁴`
appears as a hard constant in `libirtemp.dll` (`DAT_1000b4b0` = `6.103515625e-05`), used to
unscale the value in `enhance_distance_temp_correct`.

`MILI6_*.bin` additionally has a 256-byte header whose magic is `0xFFFFFFFF` at offset 4 and
whose high 16 bits are a version. Version ≤ `0x3F` keeps the 56×64 layout; `0x40`…`0x100` and
`≥ 0x101` widen it to 42×88 and 45×88 with a distance axis reaching 1000 m. The shipped tables
are version `0x0B`.

> Correction to an earlier note in this document: `blk162`/`blk163` are **not** float arrays.
> Reading `0x400F4000` as a little-endian `float` gives `2.238`, but the correct reading is two
> `uint16` values, `0x4000` (= 16384 = 1.0 in Q14) and `0x400F` (= 16399). Every row's first
> column is exactly 16384, which is what a transmittance table must look like at 0.25 m. **[V]**

Also corrected: `blk134`/`blk135` are **not** a dead-pixel/NUC mask. The `0xFFFF` run after the
first dword is the header's reserved area, and the payload is the same transmittance table in the
same Q14 encoding. **[V]**

---

## 5. Other shipped assets

| Asset | Format | Notes |
|---|---|---|
| `01-Chime.wav` … `08-Bell.wav` | RIFF/WAV | alarm sounds; referenced by `SoundIntervalSeconds` / `WarningSoundName` in `[CONFIG]` |
| `TYF0ReadMe{CN,EN}.pdf`, `TYF0CReadMe{CN,EN}.pdf`, `iScoutRead.pdf` | PDF | user manuals (Android + Windows) |
| `Color Changes.pdf`, `Rectangle.pdf`, `Polygon.pdf`, `Line.pdf`, … | PDF | feature tutorials (Windows only) |
| `form_setting.dat`, `form_setting_hide.dat`, `form_setting_{en,zh,es}.dat` | INI-ish | UI layout/state persistence |
| `ErrLog.txt` | text | runtime error log |
| `logo.png`, `modern-wizard.bmp` | image | installer/UI branding |
| `hash.txt` | text | integrity manifest |
| `vcredist_*.exe`, `NDP461-*.exe` | PE | Microsoft redistributables (irrelevant to the port) |
| `M1.exe`, `M2.exe` | PE | **[?]** purpose unknown; see `09-open-questions-and-next-steps.md` §6 |

---

## 6. Summary — what a Linux implementation must reproduce

| Format | Effort | Notes |
|---|---|---|
| Palette LUT | trivial | read 768 bytes, 256 × RGB, index 0 = cold |
| DYT container | moderate | APP2 splice; header is a fixed-offset struct table |
| `[CONFIG]` INI | trivial | plaintext keys |
| SN allow-list | not required | vendor licence check; documented for completeness |
| Calibration `.bin` | **unknown** | needs `libirtemp.dll` analysis — see §4 |
