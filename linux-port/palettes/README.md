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
the Windows installer.

## The Windows palettes

The Windows app ships 27 palettes (`Lut/lut_1.dat` … `lut_27.dat`), which exist on disk only as the
installer blocks `RE Workspace/exe/nsis_blocks/blk103.bin` … `blk129.bin`, in that order:
**`blk(102+N) = lut_N`**. That mapping is not guesswork — it is corroborated by all five palettes
the two apps share (blk114→lut_12, blk120→lut_18, blk121→lut_19, blk124→lut_22, blk126→lut_24), each
of which lands on its documented number. The installer's own NSIS file-record table, which would
confirm it independently, is still undecoded (`RE Docs/09` §3).

Of the 27, **five are byte-identical to an Android palette already present** and are not duplicated
here. The remaining **22 are tracked as `lut-NN.dat`**, named after their Windows index, so the gaps
in the numbering are exactly the five shared ramps:

| present as | Windows index |
|---|---|
| `01-iron-red.dat`  | `lut_12` |
| `05-white-hot.dat` | `lut_18` |
| `04-black-hot.dat` | `lut_19` |
| `02-rainbow.dat`   | `lut_22` |
| `03-red-hot.dat`   | `lut_24` |

So the directory holds **28 distinct ramps** — the Android app's 6 plus the Windows-only 22 — not
27, because `06-cool-blue.dat` is Android-only and the five shared ones are counted once. The
Android app's own `7.dat` ships unreferenced and is omitted, as before.

| file | source | md5 |
|---|---|---|
| `lut-01.dat` | `nsis_blocks/blk103.bin` | `445c12af7064e3886dbffe69ad8b355a` |
| `lut-02.dat` | `nsis_blocks/blk104.bin` | `6f4f1c912e79d76f41cf90eeab697e62` |
| `lut-03.dat` | `nsis_blocks/blk105.bin` | `3c9bcca4886a017692335b19c2806218` |
| `lut-04.dat` | `nsis_blocks/blk106.bin` | `219996cc33e0cecbce9deae69357792b` |
| `lut-05.dat` | `nsis_blocks/blk107.bin` | `5ca79e84d922191ec7b6fe61fcc32bc0` |
| `lut-06.dat` | `nsis_blocks/blk108.bin` | `54493683d4a044a84eb575e51f570b47` |
| `lut-07.dat` | `nsis_blocks/blk109.bin` | `5a5947d9e41c44dc9ba144224fb1bb27` |
| `lut-08.dat` | `nsis_blocks/blk110.bin` | `4c06a436030541edaa5da6c35a53367b` |
| `lut-09.dat` | `nsis_blocks/blk111.bin` | `3062ae0333d7fa9b1ef41bf769494f71` |
| `lut-10.dat` | `nsis_blocks/blk112.bin` | `22c0aefebb54c1af54e82e39e893417a` |
| `lut-11.dat` | `nsis_blocks/blk113.bin` | `ff904ab4bc803ab7742dd1169d4d63dc` |
| `lut-13.dat` | `nsis_blocks/blk115.bin` | `28ac123241111f60a26387cad2329953` |
| `lut-14.dat` | `nsis_blocks/blk116.bin` | `cff0ca2d294bc0190e196f2ab1f2debd` |
| `lut-15.dat` | `nsis_blocks/blk117.bin` | `4d5ab7f3ab1150f9d0dbb3568c811a69` |
| `lut-16.dat` | `nsis_blocks/blk118.bin` | `97764811eab62b9163af4d21f21ad55a` |
| `lut-17.dat` | `nsis_blocks/blk119.bin` | `c43c315f063149d309e6a7d2f635d6cd` |
| `lut-20.dat` | `nsis_blocks/blk122.bin` | `c8f3c5385a3917012e4119a6f0d8ef6c` |
| `lut-21.dat` | `nsis_blocks/blk123.bin` | `dd2b7a02993ba85eed4526b9f13f7e54` |
| `lut-23.dat` | `nsis_blocks/blk125.bin` | `a2d11e4e3f9ea3a00e112f9906a6c8fc` |
| `lut-25.dat` | `nsis_blocks/blk127.bin` | `49d112163df33f8167d8c8e01176a1d7` |
| `lut-26.dat` | `nsis_blocks/blk128.bin` | `7686ebc1085b1e6937934f97e0fb81e9` |
| `lut-27.dat` | `nsis_blocks/blk129.bin` | `2bbd26a6b53a6270cfd11d006dd64623` |

`dyt_palette_load_dir()` (palette.c) loads every `*.dat` in this directory in name order, so the
front-end gets all 28 without hard-coding a file list; `DYT_PALETTE_MAX` is the compile-time
storage bound.

Reproduce the extraction and verify:

```bash
cd "../RE Workspace"
md5sum apk/assets/{1..6}.dat                    # compare against the Android table above
md5sum exe/nsis_blocks/blk{103..129}.bin        # compare against the Windows table above
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
