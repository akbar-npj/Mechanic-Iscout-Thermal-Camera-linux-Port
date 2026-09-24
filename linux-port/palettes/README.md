# palettes — the vendor's own colour ramps

These are the six colour palettes the iScout app offers, extracted unchanged from the vendor
APK so the Linux viewer colours exactly like the proprietary software instead of approximating it.

## Format

**768 bytes = 256 entries × 3 bytes**, one byte per channel, stored **R, G, B**. No header, no
magic, no terminator. **Index 0 is the coldest colour, index 255 the hottest** — do not invert the
ramp again.

Derived in `RE Docs/06-asset-and-file-formats.md` §1.1–1.2 from the Android loader
`CreateBitmap.toByteArray()` (`jadx/sources/com/dyt/wcc/utils/CreateBitmap.java:25-28`), which
packs each triple into an ARGB int as `A<<24 | R<<16 | G<<8 | B`. The loader's `255 - i3` write
index is what makes file index 0 the coldest; it is a bitmap-row flip, not a ramp inversion.

## Provenance

Extracted from `RE Workspace/apk/assets/{1..6}.dat`. That directory is gitignored (it is a
disposable extraction of the vendor APK, regenerated per `RE Docs/02`), so the six files are
tracked here for the same reason `linux-port/calib/` tracks its vendor blobs: they are small,
needed to build the viewer, and re-extractable.

| file | source | app name | meaning | md5 |
|---|---|---|---|---|
| `01-iron-red.dat`   | `assets/1.dat` | `tiehong` | 铁红 iron red   | `429de09b5afb6627347af3fb6084e914` |
| `02-rainbow.dat`    | `assets/2.dat` | `caihong` | 彩虹 rainbow    | `88334c3f2bc5ee71ef796df2e57cdd16` |
| `03-red-hot.dat`    | `assets/3.dat` | `hongre`  | 红热 red hot    | `85ac61919da94d6a966117acb209c5b0` |
| `04-black-hot.dat`  | `assets/4.dat` | `heire`   | 黑热 black hot  | `4b41d71aa5545fc64575029c34ebe709` |
| `05-white-hot.dat`  | `assets/5.dat` | `baire`   | 白热 white hot  | `fc55558e8169339f09831300b068fd41` |
| `06-cool-blue.dat`  | `assets/6.dat` | `lenglan` | 冷蓝 cool blue  | `f78bd42019116e6c4a25ff09142cdfa6` |

The index → app-name mapping is from `RE Docs/06` §1.4 (`PreviewFragment.java:1805-1815`). Only
these six are referenced by `DYConstants.paletteArrays`; `7.dat` ships unreferenced and is omitted.

Five of the six are byte-identical to Windows palettes (`lut_12`, `lut_22`, `lut_24`, `lut_19`,
`lut_18` respectively, as installer blocks `blk114/124/126/121/120`). `6.dat` has no counterpart in
the Windows installer. The Windows `Lut/lut_*.dat` set (27 files) exists on disk only as
`RE Workspace/exe/nsis_blocks/blk103-129.bin`; only the six the app actually exposes are copied
here.

Reproduce the extraction and verify:

```bash
cd "../RE Workspace"
md5sum apk/assets/{1..6}.dat        # compare against the table above
cp apk/assets/1.dat ../linux-port/palettes/01-iron-red.dat   # ... etc
```

## Sanity check

`01-iron-red.dat` sampled every 32 entries — the classic iron/rainbow ramp, matching
`RE Docs/06` §1.2 exactly:

| index | R,G,B | |
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
