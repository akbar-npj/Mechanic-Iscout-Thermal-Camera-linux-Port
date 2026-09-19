# calib/ — vendor calibration tables

These four files are extracted **unmodified** from the Windows installer
(`iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe`) and are the only
calibration data the vendor ships that is not locked inside the camera.

> **These tables are Windows-SDK-only and are not on the port's critical path.**
> The shipping Android stack computes transmittance analytically
> (`CalcFixRaw`) and ships no table at all; the Windows stack is table-driven and
> contains no analytic model. The two are structurally different, so `calib.c` is
> a *second* thermometry backend, not a drop-in for the verified analytic path —
> see `RE Docs/10-calibration-tables.md` §4.5. The files are kept because which
> model better matches a given unit is still undecided.

They are reproduced here so the port has something to run against before
hardware arrives. **They are vendor data, not ours** — treat them as
read-only inputs, and prefer the per-unit values programmed into the camera
once we can read them (see RE Docs 10 §7).

Regenerate with:

```bash
cd "RE Workspace"
python3 tools/nsis_extract.py \
  "../iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe" \
  exe/nsis_blocks
python3 tools/extract_calib.py exe/nsis_blocks ../linux-port/calib
```

## What each file is

| File | Size | Shape | Meaning |
|---|---|---|---|
| `tau_H.bin` | 7168 | `uint16[56][64]` | atmospheric transmittance, high-gain path |
| `tau_L.bin` | 7168 | `uint16[56][64]` | atmospheric transmittance, low-gain path |
| `MILI6_H.bin` | 7424 | 256-byte header + `uint16[56][64]` | transmittance + emissivity-enable, high gain |
| `MILI6_L.bin` | 7424 | 256-byte header + `uint16[56][64]` | transmittance + emissivity-enable, low gain |

Row = target-temperature band (248.15 K … 1623.15 K, 25 K steps).
Column = distance band (0.25 m … 50 m, 64 entries).
Value = transmittance × 16384 (`Q14`), bilinearly interpolated.

One vendor quirk is reproduced deliberately: the first and last temperature
bands skip the interpolation entirely and return the nearest-lower distance
cell, so targets between −25 °C and 0 °C get a distance-quantised value. See
RE Docs 10 §2.4.

The full format, the index arithmetic and the source of every claim are in
`RE Docs/10-calibration-tables.md`.

## Provenance

| File | NSIS block | sha256 |
|---|---|---|
| `tau_H.bin` | `blk162` | `4c1c5db4a527a73cc900ed74442d1f8e36700b195b21244504d4fadc516da8a9` |
| `tau_L.bin` | `blk163` | `fe88a50507ae62ce6a99ad319d008fa90c5f24f311e2667606833697d3000b65` |
| `MILI6_H.bin` | `blk134` | `564f0b3374c9880a817087f6ea480d10895f2e45c70c4b09e10b7c709ecc07e4` |
| `MILI6_L.bin` | `blk135` | `ec75b1edd99519e93239fdd256b55a6146ad1975207625592c57bdce18458531` |

The installer also *names* `MILI6_H_500.bin`, `MILI6_H_91.bin`,
`MILI6_L_500.bin` and `MILI6_L_91.bin`, but only two distinct MILI6 blobs
exist in the payload — the `_91` and `_500` pairs are byte-identical and were
deduplicated by the NSIS compiler, or only one lens variant is shipped. See
RE Docs 10 §5.

The **H/L assignment** is the one inferred detail. For MILI6 it comes from bit 0
of the file header (`blk134` = `0x000B0001` → H). For tau there is no header, so
the pair is ordered by the same rule — H is the table that attenuates less. If a
device disagrees, swap the two files; nothing else depends on the names.
