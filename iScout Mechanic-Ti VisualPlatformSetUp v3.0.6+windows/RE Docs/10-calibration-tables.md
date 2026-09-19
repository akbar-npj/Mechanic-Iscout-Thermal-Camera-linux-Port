# 10 — Windows Calibration Tables (`tau_*.bin`, `MILI6_*.bin`, `block_lut.dat`)

Closes `09-open-questions-and-next-steps.md` §3 (items 3.1–3.4) for the two table
families that matter for absolute thermometry. Tags follow `README.md` §3.

**Method.** Everything below comes from headless Ghidra decompilation of the
Windows native DLLs plus a byte-level read of the extracted NSIS blocks. The
runner is `../RE Workspace/ghidra/run_win.sh`; the decompiled C is in
`../RE Workspace/ghidra/win/out/`. Where a constant is quoted, its virtual
address is given so it can be re-read from the DLL's `.data`.

---

## 0. Summary

| File | Bytes | Shape | Encoding | Loaded by | Consumed by |
|---|---|---|---|---|---|
| `tau_H.bin` | 7168 | `uint16[56][64]` | τ × 2¹⁴ | `Tiny1CDll.dll` | `read_tau_with_target_temp_and_dist` — **but see §3.4, the data is degenerate** |
| `tau_L.bin` | 7168 | `uint16[56][64]` | τ × 2¹⁴ | `Tiny1CDll.dll` | same |
| `MILI6_{H,L}.bin` | 7424 | 256-byte header + `uint16[56][64]` | τ × 2¹⁴ | `DcontrolDll.dll` | `read_compatible_tau_*` + `read_compatible_ems_*` |
| `block_lut.dat` | — | — | — | **.NET layer only** | **[?]** see §7 |

> **Scope warning.** These tables belong to the **Windows** SDK only. The Android
> build ships no table and computes τ analytically instead; the two models are
> structurally different. See **§4.5** — this decides whether the port should use
> them at all, and the answer is currently *not wired in*.

Both families store **atmospheric transmittance τ in Q14 fixed point**
(`16384` = 1.0), indexed by *target temperature* × *distance*. The scale factor
`2⁻¹⁴` is a hard constant in the DLLs: `DAT_1000b4b0` (float `6.103515625e-05`)
and `DAT_1000b4c8` (double, same value) in `libirtemp.dll`. **[V]**

The four blocks were located without relying on the NSIS filename table — see
§6 — by matching their sizes against the index arithmetic recovered from the
readers. The extraction is reproducible:

```bash
cd "../RE Workspace"
python3 tools/extract_calib.py exe/nsis_blocks ../linux-port/calib
```

---

## 1. Where the tables are loaded

### 1.1 `tau_H.bin` / `tau_L.bin` — `Tiny1CDll.dll` (blk170) **[V]**

`Tiny1CDll.dll!FUN_10011000` @ `0x10011000` — decompiled at
`ghidra/win/out/Tiny1CDll.dll.c:12998`:

```c
undefined4 FUN_10011000(char param_1)          /* param_1 = 0 -> L, !=0 -> H */
{
  if (param_1 == '\0') _Filename = "tau_L.bin";
  else                 _Filename = "tau_H.bin";
  _File = fopen(_Filename, "rb");
  ...
  fseek(_File, 0, 2);  DAT_1021a768 = ftell(_File);       /* size            */
  DAT_1021a764 = (int)malloc(DAT_1021a768);               /* whole-file blob */
  fseek(_File, 0, 0);
  fread((void *)DAT_1021a764, 1, DAT_1021a768, _File);
  fclose(_File);
  return 0;
}
```

The file is slurped whole with **no size or magic validation**. The buffer
(`DAT_1021a764`) is later passed straight to the reader, so the file layout is
defined entirely by the reader's index arithmetic.

The gain mode is a device property, and switching it re-loads the table —
`Tiny1CDll.dll!SetUVC` @ `0x10014ab0` (`Tiny1CDll.dll.c:15992`):

```c
else if (param_2 == 0x20) {          /* property 0x20 */
    if (param_1 == 0x8000) { set_prop_tpd_params(5, 1); DAT_1003a5f9 = 0; FUN_10011000(0); }  /* tau_L */
}
else if (param_2 == 0x21) {          /* property 0x21 */
    if (param_1 == 0x8000) { set_prop_tpd_params(5, 0); DAT_1003a5f9 = 1; FUN_10011000(1); }  /* tau_H */
}
else if ((param_2 == 0) && (param_1 == 0x8000)) { shutter_sta_set(1); ooc_b_update(0); }       /* FFC */
```

Note the last branch: **vendor property `0`, value `0x8000` is the FFC/shutter
trigger**, which independently corroborates `04-usb-protocol.md` §4.5.5
(`uvc_set_zoom_abs(cam, 0xffff8000)` → `0x8000`).

`DAT_1003a5f9` is the current gain flag (`1` = high gain). **[V]**

### 1.2 `MILI6_*.bin` — `DcontrolDll.dll` (blk081) **[V]**

`DcontrolDll.dll!FUN_10002fb0` @ `0x10002fb0` (`DcontrolDll.dll.c:1692`):

```c
undefined4 FUN_10002fb0(char param_1)          /* param_1 = 0 -> H, !=0 -> L */
{
  if (DAT_100248c0 == 1) {                     /* lens mode 1 */
    _Filename = "MILI6_L_91.bin";
    if (param_1 == '\0') _Filename = "MILI6_H_91.bin";
  } else {
    if (DAT_100248c0 != 2) goto LAB_10002ff4;
    _Filename = "MILI6_H_500.bin";             /* lens mode 2 */
    if (param_1 != '\0') _Filename = "MILI6_L_500.bin";
  }
  _File = _fopen(_Filename, "rb");
  ...
  DAT_1020a5ac = FUN_1000bf53(_File);          /* size */
  DAT_1020a5b0 = (void *)FUN_1000af0d(DAT_1020a5ac);
  _fread(DAT_1020a5b0, 1, DAT_1020a5ac, _File);
}
```

`DAT_100248c0` is set by `DcontrolDll.dll!ChangeCameraLensPar` @ `0x10004c90`
(`DcontrolDll.dll.c:3010`), which is the lens-variant selector (1 or 2).
The buffer `DAT_1020a5b0` is consumed by `PointTempSetting` @ `0x10004cd0`:

```c
enhance_distance_temp_correct(&local_1c, DAT_1020a5b0, DAT_1020a5ac, <target temp>, param_2);
```

so `MILI6_*.bin` is the `correct_table` argument of
`libirtemp.dll!enhance_distance_temp_correct` @ `0x100042d0`
(`libirtemp.dll.c:2171`), which uses it for **both** an emissivity correction and
a transmittance correction (§4.2, §4.3).

**Four declared names, two shipped blobs.** The NSIS string table lists
`MILI6_{H,L}_{91,500}.bin`, but only two blocks in the whole installer have the
MILI6 shape (§6). The 91/500 pair for each gain must therefore be byte-identical
and de-duplicated by the NSIS compiler, or only one lens variant is shipped.
**[I]**

---

## 2. The `tau_*.bin` format **[V]**

### 2.1 Shape

`Tiny1CDll.dll!read_tau_with_target_temp_and_dist` @ `0x1001a2f0`
(`Tiny1CDll.dll.c:20717`):

```c
int read_tau_with_target_temp_and_dist(uint table, float target_temp_c,
                                       float distance_m, ushort *out)
{
  /* --- target-temperature band --- */
  uVar3 = 0;
  do {
    dVar7 = (double)(float)(target_temp_c + 273.15);
    if (dVar7 <= DAT_10034470[uVar3] - 0.001) break;
    uVar3 = uVar3 + 1;
  } while (uVar3 < 0x40);

  /* --- distance band --- */
  FUN_10018ee0(distance_m, &dist_idx);

  /* row 0 and the last row skip the temperature interpolation — see §2.4 */
  if (row == 0 || 0x36 < row) {
      *out = table[row * 0x40 + dist_idx];
      return 0;
  }

  uVar4 = dist_idx + row * 0x40;                       /* row stride = 64    */
  *out = (short)(int)(
        table[uVar4 + 1]    * ((gT * fD) / (dT * dD))
      + table[uVar4]        * ((gT * gD) / (dT * dD))
      + table[uVar4 + 0x40] * ((fT * gD) / (dT * dD))
      + table[uVar4 + 0x41] * ((fT * fD) / (dT * dD))
      + 0.5);
}
```

* **Row = target-temperature band**, **column = distance band**, row-major,
  row stride `0x40` = 64 `uint16`. **[V]**
* 56 rows × 64 columns × 2 bytes = **7168 bytes** exactly. **[V]**
* Values are bilinearly interpolated, then rounded with `+0.5` and truncated
  (`_DAT_10028a58` = `0.5` double). **[V]**

### 2.2 Axes

Both axes are `double` arrays in `Tiny1CDll.dll`'s `.data` (identical copies
exist in `libirtemp.dll` at `DAT_1000d278` / `DAT_1000d008`). **[V]**

**Target temperature** — `DAT_10034470`, 56 entries, Kelvin:

```
248.15, 273.15, 298.15, … , 1623.15        i.e. 248.15 + 25.0·i, i = 0…55
```

That is **−25 °C … +1350 °C in 25 K steps**. The reader adds
`_DAT_10025330` = `273.15` to convert its °C argument to Kelvin. **[V]**

**Distance** — `DAT_10034200`, 64 entries, metres:

```
0.25 0.30 0.35 0.40 0.45 0.50 0.55 0.60 0.65 0.70 0.75 0.80 0.85 0.90 0.95
1.00 1.05 1.10 1.15 1.20 1.30 1.40 1.50 1.60 1.70 1.80 1.90 2.00 2.20 2.40
2.60 2.80 3.00 3.20 3.40 3.60 3.80 4.00 4.50 5.00 5.50 6.00 6.50 7.00 7.50
8.00 9.00 10.0 11.0 12.0 13.0 14.0 16.0 18.0 20.0 22.0 24.0 26.0 28.0 30.0
35.0 40.0 45.0 50.0
```

The v2/v3 variants of the same axis (`DAT_1000d438`, `DAT_1000d848`) extend this
to 88 entries up to **1000 m**; see §4.

### 2.3 Band selection

`Tiny1CDll.dll!FUN_10018ee0` @ `0x10018ee0` (`Tiny1CDll.dll.c:19798`),
`get_dist_read_index`:

```c
if (d < 0.25f || d >= 50.0f)  return -1;         /* DAT_10028a28 / _DAT_10028aa8 */
i = 0;
do { if (d <= axis[i] - 0.001) break; i++; } while (i < 0x40);
*out = i - 1;
```

So the band index is *the last breakpoint at or below the query*, with a
`0.001` (`DAT_10028a40`) slack. The temperature axis uses the identical rule.
`0.001` is a global epsilon used throughout the SDK. **[V]**

Two boundary cases are special-cased by `dyt_calib_band_index` (see §2.4):
a query below the first breakpoint returns `0`, and a query at or beyond the
last breakpoint returns `n-1`.

**Vendor edge case:** at `d == 50.0` the loop runs to `i == 64` and returns
`63`, after which the bilinear read touches `axis[64]` — one past the end. The
port takes the single-cell path instead. Separately, when the query is below the
first breakpoint the counter `uVar3` is 0, so `(ushort)(uVar3 - 1)` is `0xFFFF`
and the "last band" test `0x36 < uVar5` fires, computing the row as
`(0xFFFF & 0x3FF) * 64` — an index 65472 entries into a 3584-entry table. The
port clamps to row 0. Neither is reachable from a sane input. **[V]**

### 2.4 The row-0 degenerate case **[V]**

`Tiny1CDll.dll.c:20748` and `libirtemp.dll.c:4251` both read:

```c
if (((ushort)(uVar3 - 1) == 0) || (0x36 < uVar5)) { /* single cell */ }
```

`uVar3` is the band search counter and `uVar3 - 1` is the row, so the test is
`row == 0 || row > 54`, i.e. **the first and last temperature bands skip the
bilinear blend entirely and return the nearest-lower distance cell.**

The last-row half is sensible — there is no row 56 to blend with. The first-row
half is almost certainly a bug: the author presumably meant "the query fell
below the first breakpoint", which the search already reports as row 0, but as
written it also fires for *every* target temperature in the first band, i.e.
−25 °C … 0 °C. Those temperatures then get a distance-quantised transmittance
instead of an interpolated one.

It is shipped behaviour, it changes the answer over a real part of the
measurement range, and the port reproduces it. `calib.c` marks the condition
and `calib_test` asserts it explicitly (`test_row0_degenerate`) so that a future
"cleanup" cannot silently move the numbers.

Note that the *compatible* reader (`FUN_10006660`, used for MILI6) does **not**
have this rule — it tests only `row <= rows-2`. The port applies the row-0 rule
to both families, because for the shipped 56×64 tables the MILI6 difference is
confined to the same −25 °C … 0 °C band and the Tiny1C path is the one this
device uses. **[I]**

### 2.5 Observed data

Decoded as `uint16[56][64]`:

| block | col 0 (0.25 m) | col 63 (50 m) | interpretation |
|---|---|---|---|
| blk162 | 16384 (= 1.0000) for every row | 13696 … 14732 | less attenuation |
| blk163 | 16384 for every row | 11401 … 13225 | more attenuation |

Every row's nearest-distance column is exactly `16384` — physically required,
since at 0.25 m there is nothing to attenuate the signal. This is what the
extractor's shape test keys on. **[V]**

> **Read §3.4 before trusting anything below.** The `tau_*` pair is *not* a
> clean transmittance table: its distance variation **stops at 3.00 m** and it
> is periodic with period 14 rows. The summary numbers in this table are
> therefore misleading on their own — the "col 63" spread comes from the few
> rows that do keep varying.

Row-to-row variation (with target temperature) is small and non-monotonic, and
only 10 (`tau_H`) / 8 (`tau_L`) of the 56 rows are distinct — the buffer is
really 4 identical humidity planes × 14 rows. **[V]**

---

## 3. The `MILI6_*.bin` format **[V]**

### 3.1 Container

`libirtemp.dll!read_tau_with_target_temp_and_dist` @ `0x10006e10`
(`libirtemp.dll.c:4208`) — the version-1-only reader — and
`libirtemp.dll!FUN_10006660` @ `0x10006660` (`libirtemp.dll.c:3917`, whose own
log strings name it `read_compatible_tau_with_target_temp_and_dist`) both begin:

```c
if (*(char *)(p + 4) == -1 && *(char *)(p + 5) == -1 &&
    *(char *)(p + 6) == -1 && *(char *)(p + 7) == -1) {
      uVar1 = *p;              /* dword 0 = header  */
      p += 0x40;               /* skip 256 bytes    */
      version = (ushort)(uVar1 >> 16);
}
```

So the container is:

```
offset 0x000  uint32  header          bits 31..16 = version, bits 15..0 = flags
offset 0x004  uint32  0xFFFFFFFF      magic — presence of this switches on the header
offset 0x008  ...     reserved, all 0xFFFFFFFF up to 0x100
offset 0x100  uint16  table[target][distance]
```

Absent the magic, the file starts directly with the table (the `tau_*.bin`
layout). **[V]**

### 3.2 Version dispatch

`FUN_10006660` (`libirtemp.dll.c:3944`):

| version (`header >> 16`) | target axis | distance axis | row stride | table bytes | file bytes |
|---|---|---|---|---|---|
| magic absent | 56 (`DAT_1000d278`) | 64 (`DAT_1000d008`) | 64 | 7168 | 7168 |
| `<= 0x3F` | 56 (`DAT_1000d278`) | 64 (`DAT_1000d008`) | 64 | 7168 | 7424 |
| `0x40 … 0x100` | 42 (`DAT_1000d6f8`) | 88 (`DAT_1000d438`) | 88 | 7392 | 7648 |
| `>= 0x101` | 45 (`DAT_1000db08`) | 88 (`DAT_1000d848`) | 88 | 7920 | 8176 |

Version 3 additionally enables the emissivity sub-table (§4.3). **[V]**

The shipped tables have `header == 0x000B0001` / `0x000B0000`, i.e.
**version `0x000B` = 11**, which falls in the `<= 0x3F` row — the v1 layout with
a header, 7424 bytes. **[V]**

### 3.3 Observed data

| block | header | col 63 (50 m) | flags |
|---|---|---|---|
| blk134 | `0x000B0001` | 7619 … 8192 | bit 0 = 1 |
| blk135 | `0x000B0000` | 5669 … 5931 | bit 0 = 0 |

**Bit 0 of the header is the gain flag.** blk134 attenuates less and has bit 0
set; blk135 attenuates more and has bit 0 clear. Bit 0 is therefore the only
field that distinguishes the pair, and it matches the naming convention used
throughout the SDK. **[I]** — the decompiled loader never reads the header's low
bits, so this is inferred from the data, not proven.

### 3.4 `MILI6_*.bin` is a well-formed table; `tau_*.bin` is not **[V]**

Measured on 2026-09-20 with `dyt_calib_tau_read_f` (see §8.3 for the program).
The two families share a shape but not a structure:

| | `tau_H.bin` | `tau_L.bin` | `MILI6_H.bin` | `MILI6_L.bin` |
|---|---|---|---|---|
| distinct rows (of 56) | 10 | 8 | 6 | 23 |
| periodic with period 14 | **yes** | **yes** | no | no |
| first column where the row goes constant to the end | **col 32 (3.00 m)** | **col 32 (3.00 m)** | col 63 (never) | col 63 (never) |

Two independent anomalies, both only in the `tau_*` pair:

1. **The `tau_*` files have no valid distance information beyond ~3 m.** 12 of
   their 14 distinct rows are *bit-identical* from column 32 (3.00 m) all the way
   to column 63 (50 m). The remaining two rows hold out to 5.00 m. A
   transmittance table that does not vary with distance is not a transmittance
   table — the generator appears to have clamped at the last distance it had
   data for. `MILI6_*` shows no such plateau: it falls smoothly across the full
   0.25 m … 50 m range (e.g. `MILI6_H` at T=0 goes 1.000 → 0.651).

2. **The `tau_*` files are periodic with period 14 rows** (896 `uint16`), i.e.
   the buffer decomposes as **4 byte-identical humidity planes × 14 rows**, which
   is exactly the indexing `read_tau` uses (§4.4): 14 environment temperatures
   × 4 humidity bands. `MILI6_*` has no such periodicity, so its 56 rows are
   genuinely 56 curves.

Together these say the two files are **not the same kind of table** despite both
being 56×64 `uint16`. The `tau_*` pair carries the environment-temperature ×
humidity decomposition; `MILI6_*` carries a genuine target-temperature × distance
table.

This is awkward, because `Tiny1CDll.dll` feeds `tau_*.bin` to
`read_tau_with_target_temp_and_dist` — a **target-temperature-indexed** reader.
The reader and the data do not agree on what the row axis means. Two readings
are possible and this document does not settle it:

* the `tau_*` pair is legacy data from an older sensor/lens whose row axis really
  was environment temperature, and pointing the Tiny1C reader at it is a vendor
  bug; or
* the row axis is target temperature after all, and the coarse, plateaued data is
  simply what that generator produced.

What is *not* in doubt is the practical consequence: **`tau_*.bin` must not be
trusted for distances beyond ~3 m**, and any use of it needs the row-axis
question answered first. `MILI6_*.bin` has neither problem and belongs to the
same SDK generation as `libirtemp.dll`, which is the library that consumes it.

---

## 4. How the tables are consumed

### 4.1 The simple path (`Tiny1CDll.dll`)

`Tiny1CDll.dll!PointTempSetting` @ `0x10014790` (`Tiny1CDll.dll.c:15892`):

```c
read_tau_with_target_temp_and_dist(DAT_1021a764, param_4, 0.25f, &param_2);
temp_correct(param_1, param_2, param_3, param_4, &local_c);
read_tau_with_target_temp_and_dist(DAT_1021a764, local_c, 0.25f, &param_2);
temp_correct(param_1, param_2, param_3, param_4, uVar1);
```

Note the distance argument is the literal `0x3e800000` = **0.25 m** in both
calls — this SDK path assumes the target is at the calibration distance and only
applies the transmittance correction. **[V]**

### 4.2 The full path (`DcontrolDll.dll` → `libirtemp.dll`)

`libirtemp.dll!enhance_distance_temp_correct` @ `0x100042d0`
(`libirtemp.dll.c:2171`). `param_1` is a `float[4]` environment-correct
parameter block:

| index | meaning | range check |
|---|---|---|
| `[0]` | distance (m) | via `get_dist_read_index` |
| `[1]` | emissivity ε | `0 … DAT_10009578` (= 1.0f) |
| `[2]` | humidity (0…1) | `0 … 1.0f` |
| `[3]` | atmospheric temperature | — (logged as "tu does not affect the correction result") |

`param_2` is the MILI6 blob, `param_4` the target temperature in °C. The
sequence is: corrected ε from the EMS sub-table, then τ, then a
Stefan–Boltzmann-style inversion using `2⁻¹⁴` to unscale τ. **[V]**

### 4.3 The emissivity sub-table (version 3 only)

`libirtemp.dll!FUN_10006180` @ `0x10006180` (`libirtemp.dll.c:3746`), named by
its own log strings `read_compatible_ems_with_target_temp_and_org_ems`:

```c
if (magic absent || (header >> 16) < 0x101) { *out = org_ems; return 0; }   /* v1/v2: pass through */
target_idx = index into DAT_1000dd10[86];        /* target temp list */
ems_idx    = index into DAT_1000dc70[20];        /* original-emissivity list */
*out = (float) *(double *)(0x1000dfc0 + (ems_idx + target_idx * 0x14) * 8);
```

The correction table itself is the DLL's **own `.data` array at
`0x1000dfc0`** (`double[86][20]`), *not* read from the file — the MILI6 blob is
only consulted for its magic and version. So for the shipped version-11 tables
this correction is a no-op. **[V]**

### 4.4 The humidity interpretation

`libirtemp.dll!read_tau` @ `0x10006b20` (`libirtemp.dll.c:4099`) indexes the same
3584-entry table as

```
index = (env_idx + humidity_idx * 14) * 64 + dist_idx
```

with a 14-entry environment-temperature axis `DAT_1000d208` = `[-5, 0, 5, …,
55]` °C and a humidity index 0…3 from `FUN_10005240` / `libirtemp_048`'s
`FUN_10001c80` (`get_hum_index`, bands `< 0.3`, `< 0.5`, `< 0.7`, else). **[V]**

Note `4 × 14 = 56` — this is the *same* 56-row table, just with the row axis
re-partitioned. Since `Tiny1CDll.dll` (the SDK for the Tiny1C sensor family, which
is what this camera is) uses the target-temperature reading, the port uses
**[target temperature][distance]**. The humidity reading is retained as an
alternative for other SDK variants. **[I]**

Interestingly, the shipped `tau_*.bin` files are **periodic with period 896
`uint16`** — all four humidity planes are byte-identical, so the humidity
dimension is unpopulated in this data set. **[V]**

### 4.5 The Android build does **not** use these tables — and uses a different τ model **[V]**

This is the most consequential thing found in this deep-dive, and it is a
negative result. The two vendor builds compute atmospheric transmittance by
**two different algorithms**.

**Android / ARM (`libthermometry.so`) is analytic and table-free. [V]**

* The APK ships no calibration table of any kind: `RE Workspace/apk/assets/`
  contains only `1.dat … 7.dat` (palettes) and the PDFs/allow-list, and there is
  no `tau*`, `MILI6*` or `block_lut*` anywhere in the APK tree.
* `libthermometry.so` is **13,856 bytes** in total. The τ table alone is 7,168
  bytes — over half the library — so it cannot possibly contain one.
* `CalcFixRaw` @ `0xa90` computes τ from scratch
  (`linux-port/src/thermometry.c:66-123`):

  ```
  τ     = humi · exp(1.5587 + 0.06939·air − 2.7816e-4·air² + 6.8455e-7·air³)
  e1    = exp(−√d · (0.006569 − 0.002276·√τ))
  e2    = exp(−√d · (0.012620 − 0.006670·√τ))
  τ_eff = 1.9·e1 − 0.9·e2
  ```

  This is the textbook two-band atmospheric-transmittance fit, evaluated in
  closed form. No memory is read.

**Windows / x86 (`libirtemp.dll`) is table-driven only. [V]**

* Searching the decompiled `libirtemp.dll` for every coefficient above
  (`0.006569`, `0.002276`, `0.012620`, `0.006670`) returns **zero hits**.
* The analytic form needs `exp()` twice, and `libirtemp.dll` contains exactly
  **one** real `libm_sse2_exp_precise` call site (line 2323 of the decompilation;
  the other matches are the function's own definition). That call is inside
  `calculate_vapor_pressure`, the Magnus-form saturation-vapour-pressure term —
  **not** transmittance. There is no other exponential in the library.
* Every τ it uses therefore comes from the loaded blob: `read_tau` (humidity ×
  env-temp indexed, §4.4) or `read_tau_with_target_temp_and_dist`
  (target-temp × distance indexed, §2.3).

So the table is a **Windows-SDK-only** mechanism. The Windows SDK is shared
across sensor families (`ASIC_384_640_1280`, §3.2) and ships per-lens tables,
which is consistent with a table-driven design; the Android library is built for
one sensor family and hard-codes the fit.

**Consequence for the port.** `tools/thermometry_diff` is byte-verified against
the ARM library, so the port follows the ARM path and the analytic τ is the one
in the pipeline. `calib.c` therefore exists as an **independent, unwired
module**: it is correct and fully tested, but nothing calls it yet. Wiring it in
is *not* a drop-in substitution — `libirtemp` feeds the Q14 integer into its own
`reverse_temp_correct`/Stefan–Boltzmann chain, which is structured differently
from the ARM pipeline — so it would be a second, parallel thermometry backend
rather than a swap of one function.

**Which is right for this unit is unresolved [ ? ]** and cannot be settled
statically. The two models differ in *form*, not just in constants, so they will
not agree to the last count. The decisive test needs the hardware — see **§8.4**
for the design, which a dry run has already validated and which requires a
**300 °C** source sampled at **1 m, 2 m and 10 m**. Until then the ARM model is
the default because it is the only one with byte-exact ground truth.

---

## 5. Which file is `H` and which is `L` **[I]**

* For **MILI6**, the answer is in the file: bit 0 of the header. blk134 =
  `0x000B0001` → H, blk135 = `0x000B0000` → L. This is corroborated by the
  transmittance values (H attenuates less).
* For **tau**, there is no header. The port orders the pair by the same rule the
  MILI6 header states (H = the table with the larger transmittance at the
  farthest distance), which gives `blk162 → tau_H`, `blk163 → tau_L`.

This is the one genuinely unresolved detail, and it is cosmetic:
`dyt_calib_load()` takes an explicit path, so a caller can point at whichever
file it likes, and swapping the two changes nothing else. `calib/README.md`
records the assignment and how to reverse it.

---

## 6. How the blocks were identified (no filename table needed)

The NSIS header's file-record table could **not** be decoded. The decompressed
header (`exe/nsis_blocks/_nsis_header.bin`) has a monotone block table at
`0x04`:

| block | offset | count |
|---|---|---|
| 0 | `0x012C` | 5 |
| 1 | `0x026C` | 6 |
| 2 | `0x32FC` | 803 |
| 3 | `0xA3E8` | 0 |
| 4 | `0xFB92` | 3 |
| 5 | `0xFF70` | 0 |

Block 0 is the language table and block 3 is the UTF-16 string table (base
`0xA3E8`, alphabetically sorted — which is exactly why
`07-windows-app-and-installer.md` §2.1 warns against order-based alignment).
Block 2 spans `0x32FC … 0xA3E8` = 28908 bytes = 803 × 36, and is presumably the
file records, but none of the obvious field layouts (16-byte or 36-byte stride,
absolute or string-table-relative name offsets) reproduces the known name
offsets. **This remains [?].**

Identification was therefore done structurally instead, which is stronger
evidence than a name table would have been: the readers' index arithmetic
demands a table of exactly `56 × 64` `uint16`, and the only blocks in the entire
installer with that shape are blk162/blk163; the only blocks with the
256-byte `0xFFFFFFFF` header plus that table are blk134/blk135. **[V]**

```
$ python3 - <<'PY'
import glob, os, struct
for p in sorted(glob.glob('blk*.bin')):
    d = open(p, 'rb').read()
    if len(d) == 7168:
        print(p, 'tau-shaped')
    if len(d) == 7424 and d[4:256] == b'\xff'*252:
        print(p, 'MILI6-shaped, header %#010x' % struct.unpack_from('<I', d, 0)[0])
PY
blk134.bin MILI6-shaped, header 0x000b0001
blk135.bin MILI6-shaped, header 0x000b0000
blk162.bin tau-shaped
blk163.bin tau-shaped
```

### 6.1 SHA-256 of the extracted tables

```
4c1c5db4a527a73cc900ed74442d1f8e36700b195b21244504d4fadc516da8a9  tau_H.bin
fe88a50507ae62ce6a99ad319d008fa90c5f24f311e2667606833697d3000b65  tau_L.bin
564f0b3374c9880a817087f6ea480d10895f2e45c70c4b09e10b7c709ecc07e4  MILI6_H.bin
ec75b1edd99519e93239fdd256b55a6146ad1975207625592c57bdce18458531  MILI6_L.bin
```

---

## 7. Still open

| Item | Status |
|---|---|
| `block_lut.dat` format | **[?]** — the only reference is the UTF-16 string `block_lut.dat` in `blk075` (`CA30D.exe`, a .NET assembly). No native code references it, and no block in the payload matches a plausible LUT shape. Decoding it needs a .NET decompiler (ILSpy/dnSpy), which is not installed here. |
| `hash.txt` | **[?]** — declared at the install root (`\hash.txt`) but absent from the payload, so it is generated at install time. No block references it. |
| `91` vs `500` lens variant | **[?]** — selected by `ChangeCameraLensPar()`; the shipped blobs do not encode which they are. |
| NSIS file-record table | **[?]** — see §6. Only needed for name→block attribution, which §6 replaces. |
| `DAT_1000dfc0` EMS table | **[I]** — a `double[86][20]` in `libirtemp.dll`'s `.data`. Extractable, but only used for table versions ≥ `0x101`, which the shipped tables are not. |
| `calculate_tau` | **[I]** — `libirtemp.dll` @ `0x100041a0` decompiles to `min(2.0, ((a−b)/(c−d))·K)` with a call Ghidra mis-renders as `libm_sse2_pow_precise()`. It is the *write* path (recalibration), not the read path, so it does not affect the port. |
| ARM vs Windows τ model | **[?]** — the Android build computes τ analytically and ships no table, the Windows build is table-only (§4.5). Which matches this unit's factory calibration is undecidable without hardware. Resolve with a two-distance blackbody comparison. |

---

## 8. Port status

`linux-port/src/calib.{h,c}` implements §2 and §3, including the version
dispatch, the `0.001` band slack, the row-0 degenerate rule (§2.4), the `+0.5`
rounding, and the Q14 scaling. `linux-port/src/calib_test.c` runs 14,904 checks
and is wired into `make check`.

**The module is complete but deliberately unwired.** Per §4.5 the shipping
Android stack does not use these tables, so `calib.c` is not on the
`raw frame → temperatures` path; the pipeline uses the byte-verified analytic
`CalcFixRaw` (`thermometry.c`). `calib.c` is the foundation for a second,
table-driven thermometry backend if the hardware comparison shows the Windows
model is the better fit for this unit.

The test suite has four layers, deliberately in increasing order of
circularity:

1. **Structural invariants** that hold for any correctly parsed table and do
   not depend on our reading of the arithmetic at all — every row's
   nearest-distance cell is exactly `16384`, all 3584 cells are within a sane
   Q14 range, and a query landing on a breakpoint returns that cell exactly.
   These catch a wrong payload offset, a transposed table or a wrong stride.
2. **Range behaviour** — the four out-of-range cases, three of which collapse
   to a single cell and can be asserted against the table directly.
3. **The row-0 rule** (§2.4), asserted explicitly so a future cleanup cannot
   silently move the numbers.
4. **Frozen vectors** — eight interior points per file, produced by an
   independent Python transcription of the decompiled reader. These pin the
   interpolation arithmetic and the rounding, but they are regression anchors,
   not proof: they come from the same reading of the disassembly that
   `calib.c` implements. Layer 1 is what makes the pair meaningful.

### 8.1 Frozen vectors

| file | T (°C) | d (m) | raw Q14 |
|---|---|---|---|
| `tau_H.bin` | 25 | 1.0 | 15723 |
| | 25 | 3.0 | 14732 |
| | 60 | 2.5 | 14796 |
| | 150 | 7.0 | 14024 |
| | 400 | 12.0 | 14499 |
| | 1000 | 25.0 | 13696 |
| | 12.5 | 0.625 | 16068 |
| | 87.5 | 1.75 | 14928 |
| `tau_L.bin` | 25 | 1.0 | 14859 |
| | 25 | 3.0 | 13225 |
| | 60 | 2.5 | 13413 |
| | 150 | 7.0 | 12910 |
| | 400 | 12.0 | 13169 |
| | 1000 | 25.0 | 11401 |
| | 12.5 | 0.625 | 15503 |
| | 87.5 | 1.75 | 14282 |
| `MILI6_H.bin` | 25 | 1.0 | 15041 |
| | 25 | 3.0 | 13926 |
| | 60 | 2.5 | 14076 |
| | 150 | 7.0 | 11977 |
| | 400 | 12.0 | 10650 |
| | 1000 | 25.0 | 8913 |
| | 12.5 | 0.625 | 15532 |
| | 87.5 | 1.75 | 14332 |
| `MILI6_L.bin` | 25 | 1.0 | 15237 |
| | 25 | 3.0 | 14139 |
| | 60 | 2.5 | 14348 |
| | 150 | 7.0 | 11960 |
| | 400 | 12.0 | 8667 |
| | 1000 | 25.0 | 6234 |
| | 12.5 | 0.625 | 15647 |
| | 87.5 | 1.75 | 14733 |

### 8.2 Behaviour outside the axes **[V]**

Three of the four out-of-range cases are clamped by the band search and give a
single cell. The fourth — **a distance below 0.25 m** — is *not* clamped by the
vendor's compatible reader: it selects band 0 and then bilinearly extrapolates
using the first cell, which yields a transmittance **above** 1.0. That is
physically nonsense but it is what the code does, so the port reproduces it and
the test pins the exact extrapolated values (16749 / 17049 / 16549 / 17204 for
H-tau / L-tau / H-MILI6 / L-MILI6 at 0 m). The Tiny1C path is unaffected: its
`get_dist_read_index` rejects `d < 0.25` outright. **[V]**

`block_lut.dat` is **not** needed for the port: nothing in the native
thermometry path reads it (§7).

### 8.3 Comparing the two models without hardware

The three candidate transmittance models can be evaluated side by side with no
device, using the real port code. `linux-port/tools/taucmp.c` is that harness;
it produced §3.4 and the tables below.

```bash
cd linux-port
make build/taucmp
./build/taucmp -d calib                          # default: tau sweep + row structure
./build/taucmp -d calib --air 30 --humi 0.8      # override ambient / humidity
./build/taucmp -d calib --sim 300                # dry-run the experiment (§8.4)
```

**Row structure — the decisive measurement:**

| table | distinct rows | period-14 | rows varying to the last column | rows flat from 3.00 m | rows flat from 5.00 m |
|---|---|---|---|---|---|
| `tau_H.bin` | 10 | **yes** | **0** | 48 | 8 |
| `tau_L.bin` | 8 | **yes** | **0** | 48 | 8 |
| `MILI6_H.bin` | 6 | no | **56** | 0 | 0 |
| `MILI6_L.bin` | 23 | no | **56** | 0 | 0 |

**Transmittance at ambient 25 °C** (analytic at `humi = 0.5`):

| distance | analytic `tau_eff` | `tau_H` (T=0) | `MILI6_H` (T=0) | `MILI6_H` (T=300) |
|---|---|---|---|---|
| 0.25 m | 0.9966 | 1.0000 | 1.0000 | 1.0000 |
| 0.50 m | 0.9952 | 0.9957 | 0.9630 | 0.9500 |
| 1.00 m | 0.9932 | 0.9597 | 0.9180 | 0.9030 |
| 2.00 m | 0.9903 | 0.9289 | 0.8750 | 0.8540 |
| 3.00 m | 0.9881 | 0.8992 | 0.8500 | 0.8250 |
| 5.00 m | 0.9846 | 0.8992 ← flat | 0.8210 | 0.7880 |
| 10.0 m | 0.9781 | 0.8992 ← flat | 0.7660 | 0.6770 |
| 20.0 m | 0.9687 | 0.8992 ← flat | 0.7140 | 0.5750 |
| 50.0 m | 0.9497 | 0.8992 ← flat | 0.6510 | 0.4650 |

Two things follow. First, the models are **far apart** — at 50 m they span
0.465 … 0.950, a factor of two — so a hardware comparison has ample signal.
Second, the analytic model's `tau_eff` has **no target-temperature argument at
all**, while both tables are indexed by it. The models therefore cannot be
compared at a single point; any measurement must state which target-temperature
row it is comparing against.

### 8.4 The two-distance experiment, restated

The original framing ("measure at 0.5 m and 3 m, see which backend matches") is
**not well-posed** as written, for the reasons in §3.4 and §8.3:

* 3.00 m is exactly where `tau_*.bin` stops varying, so a 3 m sample probes the
  degenerate region of that table.
* The tables are target-temperature indexed and the analytic model is not, so
  "which matches" only has an answer per target temperature.

**Dry run.** `taucmp --sim` propagates each model's τ through a Stefan–Boltzmann
radiance inversion to get the *apparent* temperature a blackbody would show, and
reports whether the models are separable above an assumed noise floor. This is a
**sensitivity calculation for planning, not a measurement prediction** — it uses
the port's `L ∝ T⁴` convention, not the vendor's full inversion.

```bash
./build/taucmp -d calib --sim 300 --dist 0.5,2,10
./build/taucmp -d calib --sim 300 --noise 0.5      # pessimistic repeatability
```

The discriminator is the apparent-temperature **drop from 2 m to 10 m**. Absolute
accuracy is irrelevant; the differential cancels emissivity and absolute
calibration error.

**Result — the experiment works, but only with a hot target.** Assumed
repeatability 0.2 K, ambient 25 °C, `humi` 0.5, at 2 m → 10 m:

| target | analytic | `tau_H` | `MILI6_H` | closest pair vs noise | verdict |
|---|---|---|---|---|---|
| 50 °C | +0.27 K | +0.52 K | +2.52 K | 0.25 K = **1.2×** | **not separable** |
| 100 °C | +0.68 K | +1.12 K | +7.59 K | 0.44 K = **2.2×** | marginal |
| 200 °C | +1.23 K | +2.16 K | +20.84 K | 0.93 K = 4.7× | viable |
| **300 °C** | **+1.64 K** | **+9.98 K** | **+28.32 K** | **8.34 K = 41.7×** | **decisive** |
| 500 °C | +2.34 K | +6.81 K | +40.79 K | 4.47 K = 22.3× | decisive |

At 100 °C — a plausible "hot plate" target — the analytic and `tau_H` predictions
are only 2.2× the noise floor apart, which is not enough to conclude anything.
**Use a 300 °C source.** That widens the closest gap to 8.3 K, or 42× the noise.

The span matters too, and not monotonically (wider is not always better). At
300 °C, using a 1 m → 10 m pair instead of 2 m → 10 m widens the gap to 13.0 K
(65×). Longer spans such as 5 m → 20 m are *worse* (10.7×) because the analytic
and `tau_H` drops converge there.

**Corrected design, for when the device arrives:**

1. Blackbody or stable high-emissivity source at **300 °C**. Record the setpoint
   accurately — the predicted drops scale with (target − ambient).
2. Hold ambient and humidity fixed and **record both**; the analytic model needs
   them and `humi` is passed as a 0…1 fraction.
3. Sample at **1 m, 2 m and 10 m**. The 2 m → 10 m pair is the discriminator;
   1 m → 10 m is the stronger variant; 1 m also gives a near-unity reference.
4. Take several frames per distance and average — the measurement needs
   *repeatability*, not absolute accuracy.
5. Compare the measured drop against the table above. The three models predict
   ~1.6 K, ~10 K and ~28 K; nothing in the setup is within 8 K of the wrong
   answer.

One caveat that the dry run also exposed: `tau_H`'s predicted drop is
**non-monotonic** in target temperature (+9.98 K at 300 °C but +6.81 K at 500 °C).
That is a direct fingerprint of the distance plateau — at 300 °C the row selected
happens to hold out to 5 m, at 500 °C it plateaus at 3 m. Any `tau_*` result
should be read with §3.4 in mind rather than taken at face value.

Until the experiment is run, the analytic ARM path stays the port's default
because it is the only one with byte-exact ground truth.

