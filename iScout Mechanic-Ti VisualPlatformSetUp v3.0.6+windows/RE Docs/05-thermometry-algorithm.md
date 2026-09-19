# 05 — Thermometry Algorithm (`libthermometry.so`)

> **Primary porting reference.** `libthermometry.so` is 13 856 bytes, 46 functions,
> plain C, no JNI. It was decompiled in full. Everything below is **[V]** unless tagged
> otherwise. Source file name embedded in the binary: **`jni/libthermometry/thermometry.c`**.

## 5.1 Exported API **[V]**

```
GetTempEvn                CalcFixRaw               InitTempParam
GetFix                    distanceFix
thermometryT              thermometryT4Line
thermometrySearch         thermometrySearchSingle  thermometrySearchCMM
```

`thermometryT` is the core LUT builder; `thermometryT4Line` is the same computation for a
single line; `thermometrySearch*` look temperatures up from the built LUT.

---

## 5.2 Data model

* **Raw thermal samples are 14-bit**, stored as `uint16`, valid `0 … 16383` (`0x4000`).
  Enforced by range checks throughout (`if (value < 0x4000)`). **[V]**
* `thermometryT` builds a **lookup table of 16 384 `float` values**, indexed by raw sample.
  The fill loop runs `i = -base … 0x4000 - base`, writing one float per raw code. **[V]**
* `thermometrySearch` then maps raw samples to temperatures:
  `temp = lut[raw] + offset`. **[V]**
* `thermometrySearch` rejects a measurement point if any of the six sampled raw values is
  `>= 0x4000` (`"thermometrySearch err data"`). **[V]**

**Consequence for the port:** you only need the LUT builder, not a per-pixel transcendental
evaluation. Build a 16 384-entry `float` LUT once per parameter change and index it.

**There is a second, LUT-free path.** For devices whose mode is `1000` / `0x3eb` (see
`04-usb-protocol.md` §4.10), the conversion is a plain affine map with no reference band and no
LUT:

```c
T_celsius = (float)raw / 64.0f - 273.15f;   /* raw is Kelvin x 64, Q6 fixed point */
```

implemented as `FrameImage::adValueArray2FloatTempArray` (`libUVCCamera.so @ 0x182978`). It
produces a `w × h` image rather than `w × (h−4)`. **[V]**

---

## 5.3 The six parameters

The per-frame "user area" supplies the parameters (see `04-usb-protocol.md` §4.5). Their
identities are pinned by the log string in `thermometryT4Line`:

```c
__android_log_print(..., "libUVCCamera",
  "[%d*%s:%d:%s]:correction:%f, Refltmp:%f,Airtmp:%f ,humi:%f,emiss:%f,distance:%d\n",
  ...);
```

| Parameter | Meaning | Type | Source offset in user area |
|-----------|---------|------|---------------------------|
| `correction` | additive correction (°C) | float | `+0xf8` |
| `Refltmp` | reflected (background) temperature °C | float | `+0xfc` |
| `Airtmp` | ambient / air temperature °C | float | `+0x100` |
| `humi` | relative humidity (0…1) | float | `+0x104` |
| `emiss` | emissivity (0…1) | float | `+0x108` |
| `distance` | distance (units: see §5.7) | uint16 | `+0x10c` |

Plus **five calibration coefficients** read at `+0x00, +0x04, +0x08, +0x0c, +0x10` of the
user area (named `a, b, c, d, e` below). **[V]**

> Offsets are relative to the row/parameter base computed in `thermometryT`; see
> `04-usb-protocol.md` §4.5 for the width-dependent strides. The **absolute frame layout is
> still unconfirmed [?]** — only the relative offsets are known.

---

## 5.4 Core formulas

### 5.4.1 `GetTempEvn` — quartic inversion

```c
float GetTempEvn(float T_C, float A, float B)   /* A=local_c, B=local_18 */
{
    double d = pow((double)(T_C + 273.15f), 4.0);
    d = pow((double)(((float)d - A) * B), 0.25);
    return (float)d - 273.15f;
}
```

i.e.

```
GetTempEvn(x, A, B) = ( ((x + 273.15)^4 - A) * B )^(1/4) - 273.15
```

This is the Stefan–Boltzmann relation: radiance ∝ T⁴. `A` is the background radiance
term and `B` the reciprocal transmittance·emissivity product. Constant `273.15` is at
`.rodata 0xa18`. **[V]**

### 5.4.2 `InitTempParam` — completing the square

```c
void InitTempParam(float a, float b, float *out1, float *out2)
{
    *out1 = b / (2.0f * a);
    *out2 = (b * b) / (4.0f * a * a);      /* == (*out1)^2 */
}
```

### 5.4.3 `CalcFixRaw` — atmospheric / emissivity model

```c
/* param_1 = Airtmp (°C)      param_2 = humidity
   param_3 = distance         param_4 = emissivity
   param_5 = Reflected temp (°C)                                  */
void CalcFixRaw(float air, float humi, float dist, float emiss,
                float refl, float *tau, float *tau_eff,
                float *inv_tau_emiss, float *bg)
{
    /* 1. humidity-corrected atmospheric transmittance before range term */
    double d1 = exp( 1.5587f
                   + 0.06939f   * air
                   - 2.7816e-4f * air*air
                   + 6.8455e-7f * air*air*air );

    *tau = humi * (float)d1;                     /* out6 (local_20) */

    /* 2. range-dependent transmittance, two empirical exponentials */
    float  sq_dist = sqrtf(dist);
    float  sq_tau  = sqrtf(*tau);

    double e1 = exp( -sq_dist * ( 0.006569f + (-0.002276f) * sq_tau ) );
    double e2 = exp( -sq_dist * ( 0.012620f + (-0.006670f) * sq_tau ) );

    *tau_eff = (float)( e2 * (-0.9) + 1.9 * e1 );   /* out7 (local_1c) */

    /* 3. reciprocal of (transmittance * emissivity) */
    *inv_tau_emiss = 1.0f / (*tau_eff * emiss);      /* out8 (local_18) */

    /* 4. background radiance term, in (K)^4 */
    double L_refl = pow((double)(refl + 273.15f), 4.0);
    double L_air  = pow((double)(air  + 273.15f), 4.0);

    *bg = (float)( L_air * (1.0 - *tau_eff)
                 + *tau_eff * L_refl * (1.0 - emiss) );   /* out9 (local_c) */
}
```

`*bg` is exactly the standard radiometric background term
`(1−τ)·L(T_air) + τ·(1−ε)·L(T_refl)` with `L(T) = (T+273.15)⁴`. **[V]**

### 5.4.4 `GetFix` — reference-pixel offset

```c
int GetFix(float T, int mode, int width)
{
    if (mode != 0x78) return 0;
    if (width != 0x100) {
        short s = (short)(390.0f - 7.05f * T);
        return s < 0 ? 0 : s;
    }
    return 0xAA;              /* 170 */
}
```

Constants `390.0` and `7.05` are floats at `.rodata 0xa64` and `0xa60`. **[V]**

### 5.4.5 `distanceFix` — distance/ambient compensation

```c
float distanceFix(float T, float dist, float air, float fallback, int mode)
{
    if (mode == 0x44) {
        double d = 18.125;
        if (dist < 20.0f) d = 0.85 * dist + 1.125;
        return T + (float)( d * (T - air) / 100.0 );
    }
    if (mode == 0x82) {
        if (dist < 20.0f)
            return T + (float)( ((0.85*dist - 1.125) * (T - air)) / 100.0 );
        return T + (float)( (15.875 * (T - air)) / 100.0 );
    }
    return fallback;
}
```

**[V]** This is inlined verbatim into `thermometryT` / `thermometryT4Line`.

---

## 5.5 The main LUT builder `thermometryT` (faithful pseudo-C)

Reconstructed from the decompilation. Names are ours; control flow and constants are exact.

```c
/* param_1  T_ambient_in  (°C, adjusted inside)
   param_2  width         (0xf0 | 0x100 | 0x180 | 0x280)
   param_3  row index
   param_5  frame pointer (uint16 pixels + appended user area)
   param_13 sensor mode   (0x44 | 0x82 | other)
   param_14 "fix mode"    (0x78 enables GetFix)
   param_4  OUT: LUT[0..16383] of float temperatures                */
void thermometryT(float T_ambient_in, int width, int row,
                  float *lut, long frame, float *ambient_out,
                  float *corr, float *refl, float *air, float *humi,
                  float *emiss, ushort *dist, int sensor_mode, int fix_mode)
{
    /* --- 1. read reference pixel & compute board/ambient estimate --- */
    ushort raw_ref = *(ushort *)(frame + ((row - 4) * width + 1) * 2);

    float scale, off;
    switch (width) {
      case 0x100: scale = 37.682f; off = 0x21A9; break;   /* 8617 */
      case 0x0f0: scale = 36.0f;   off = 0x1E78; break;   /* 7800 */
      case 0x180: scale = 36.0f;   off = 0x1E78; break;
      case 0x280: scale = 33.8f;   off = 0x1AD3; break;   /* 6867 */
      default: goto skip;
    }
    *ambient_out = 20.0f - (float)((int)raw_ref - off) / scale;

skip:
    /* --- 2. pull calibration coefficients & parameters from user area --- */
    long p = (long)((row - 4) * width + 3) * 2;      /* byte offset */
    float a = *(float *)(frame + p + 0x00);
    float b = *(float *)(frame + p + 0x04);
    float c = *(float *)(frame + p + 0x08);
    float d = *(float *)(frame + p + 0x0C);
    float e = *(float *)(frame + p + 0x10);
    *corr  = *(float *)(frame + p + 0xF8);
    *refl  = *(float *)(frame + p + 0xFC);
    *air   = *(float *)(frame + p + 0x100);
    *humi  = *(float *)(frame + p + 0x104);
    *emiss = *(float *)(frame + p + 0x108);
    *dist  = *(ushort*)(frame + p + 0x10C);

    /* --- 3. distance used by the model --- */
    float distv = (sensor_mode == 0x44) ? (float)(*dist * 3)
               : (sensor_mode == 0x82) ? (float)(*dist)
               :                         (float)(*dist);

    /* --- 4. parameter preparation --- */
    float half, quarter;
    InitTempParam(a, b, &half, &quarter);          /* half=b/2a, quarter=(b/2a)^2 */

    float tau, tau_eff, inv_te, bg;
    CalcFixRaw(*air, *humi, distv, *emiss, *refl,
               &tau, &tau_eff, &inv_te, &bg);

    /* --- 5. ambient in Celsius from the reference pixel --- */
    T_ambient_in = ((float)((int)raw_ref_something) / 10.0f - 273.15f) + T_ambient_in;
    float amb2 = *ambient_out;

    int fix = GetFix(amb2, fix_mode, width);
    unsigned base = (unsigned)((int)raw_ref - fix) & 0xFFFF;

    /* --- 6. two quadratics in the ambient estimate --- */
    float q1 = b * T_ambient_in + T_ambient_in * a * T_ambient_in;
    float q2 = amb2 * d + amb2 * amb2 * c + e;

    /* --- 7. fill the LUT --- */
    int i = -(int)base;
    float *out = lut;
    do {
        double inner = (double)(( q1 + q2 * (float)i ) / a + quarter);
        double s     = sqrt(inner);                 /* NaN-guarded in the original */
        float  T     = GetTempEvn((float)(s - half), bg, inv_te);

        if (sensor_mode == 0x44) {
            if (distv < 20.0f)
                *out = T + (float)( ((distv * 0.85f + 1.125) * (T - *air)) / 100.0 );
            else
                *out = T + (float)( ((T - *air) * 18.125) / 100.0 );
        } else if (sensor_mode == 0x82) {
            if (distv < 20.0f)
                *out = ((0.85f * distv - 1.125f) * (T - *air)) / 100.0f + T;
            else
                *out = ((T - *air) * 15.875f) / 100.0f + T;
        } else {
            if (distv < 20.0f)
                *out = ((0.85f * distv - 1.125f) * (T - *air)) / 100.0f + T;
            else
                *out = ((T - *air) * 15.875f) / 100.0f + T;
        }
        out++; i++;
    } while (i != 0x4000 - (int)base);
}
```

> Step 5 in the original is
> `param_1 = ((float)uVar11 / 10.0f - 273.15f) + param_1;`
> where `uVar11` is a `uint16` read from the frame **after** the reference pixel. Treat it
> as a second on-frame reference value. Its exact provenance is **[?]**.

### Simplification worth noting

Because the loop body is linear in `i` and only `sqrt` + `pow` are transcendental, the LUT
can be built with **16 384 `sqrt` + `pow` calls** — a few milliseconds. No approximation is
needed on modern hardware.

---

## 5.6 Complete constant table **[V]**

All read directly from `libthermometry.so`. Ghidra VA → file offset:
`file_off = VA - 0x100000` (first PT_LOAD); subtract a further `0x10000` for VA ≥ `0x112d48`.

> **Byte-verified 2026-09-19** by dumping the rodata with Python
> (`struct.unpack` of the raw `.so` bytes). Every float and double in this
> table matches to 10 significant figures. The doubles at `0xc80`/`0xc88` and
> `0x1be0`/`0x1be8` are stored as 8-byte IEEE-754 doubles, **not** pairs of
> floats — reading them as two f32 yields the nonsense values documented in
> the trap note below.

### `GetTempEvn`
| VA | Type | Value |
|----|------|-------|
| `0x100a18` | float | `273.15` |

### `GetFix`
| VA | Type | Value |
|----|------|-------|
| `0x100a60` | float | `7.05` |
| `0x100a64` | float | `390.0` |

### `CalcFixRaw`
| VA | Type | Value | Role |
|----|------|-------|------|
| `0x100c60` | float | `6.8455e-07` | air-temp cubic |
| `0x100c64` | float | `2.7816e-04` | air-temp quadratic |
| `0x100c68` | float | `0.06939` | air-temp linear |
| `0x100c6c` | float | `1.5587` | air-temp constant |
| `0x100c70` | float | `-0.002276` | exp-1 slope |
| `0x100c74` | float | `0.006569` | exp-1 intercept |
| `0x100c78` | float | `-0.006670` | exp-2 slope |
| `0x100c7c` | float | `0.012620` | exp-2 intercept |
| `0x100c80` | **double** | `-0.9` | weight of exp-2 |
| `0x100c88` | **double** | `1.9` | weight of exp-1 |
| `0x100c90` | float | `273.15` | |

> **Trap:** `0x100c80` and `0x100c88` are **doubles**. Reading them as floats yields
> `-2.0` and `3.69e19`, which is nonsense. The neighbouring "floats" at `0x100c84`
> (`-1.85`) and `0x100c8c` (`1.9875`) are just the high halves of those doubles.

### `thermometryT` / `thermometryT4Line`
| VA | Type | Value | Role |
|----|------|-------|------|
| `0x101344` / `0x101bc4` | float | `36.0` | reference scale (w=0xf0, 0x180) |
| `0x101348` / `0x101bc8` | float | `37.682` | reference scale (w=0x100) |
| `0x10134c` / `0x101bcc` | float | `33.8` | reference scale (w=0x280) |
| `0x101350` / `0x101bd0` | float | `273.15` | |
| `0x101354` / `0x101bd4` | float | `15.875` | distance term (≥20) |
| `0x101358` / `0x101bd8` | float | `100.0` | divisor |
| `0x10135c` / `0x101bdc` | float | `0.85` | distance slope (<20) |
| `0x101360` / `0x101be0` | **double** | `18.125` | distance term, mode 0x44 (≥20) |
| `0x101368` / `0x101be8` | **double** | `100.0` | divisor |

Reference-pixel offsets: `0x21A9` (w=0x100), `0x1E78` (w=0xf0 and 0x180), `0x1AD3` (w=0x280).

### `distanceFix`
| VA | Type | Value |
|----|------|-------|
| `0x102148` | double | `18.125` |
| `0x102150` | double | `15.875` |
| `0x102158` | double | `0.85` |
| `0x102160` | double | `100.0` |

---

## 5.7 Open questions

1. **Units of `distance` (the frame value at `rec+0x112`).** The `*3` scaling
   for `sensor_mode == 0x44` and the `< 20.0` threshold suggest the raw u16 is
   in cm or in 0.1 m. Note this is **not** `param_14` — see §5.8's correction.
   The vendor log's `distance:%d` prints this frame value. **[?]**
2. **Meaning of `sensor_mode` 0x44 vs 0x82 vs other.** Three distinct branches with
   different distance weighting. Likely sensor revisions or temperature sub-ranges. **[?]**
3. **`fix_mode == 0x78`** (= `param_14`). Only then is `GetFix` non-zero. Likely
   "fixed-pattern noise correction enabled". **[V]** (verified at `0x1620`; any
   other value → `GetFix` returns 0.)
4. ~~**Provenance of the 5 calibration coefficients `a…e`.**~~ **RESOLVED [V].** They arrive in the
   frame's `userArea` reference band, at fixed offsets from a width-dependent record base. The
   device computes them per frame; they are **not** read from the Windows `.bin` files. The exact
   offsets are tabulated in `04-usb-protocol.md` §4.5.2. Whether the *device* derives them from
   its own flash tables (`MILI6_*.bin`, `tau_*.bin`) or from an on-die calibration is still
   open — see `09-open-questions-and-next-steps.md` §3. **[?]**
5. ~~**`param_3` (row index) semantics**~~ **RESOLVED [V].** `thermometryT4Line` is called **once
   per frame**, not per row. The frame pointer it receives is
   `raw + width * (height - 4)` — the start of the **last 4 rows**, which are a dedicated
   reference / shutter band, not image data. The "row index" parameter is the **total** frame
   height (active + 4), and the reference pixel is the **second `uint16` of that band**
   (`userArea[1]`). See `04-usb-protocol.md` §4.5.1–§4.5.2. **[V]**

---

## 5.8 The frame-side contract — what the port must supply **[V]**

Everything `libthermometry` needs is now pinned down. A Linux port can call the same functions
with:

| argument | value | source |
|---|---|---|
| `T_ambient_in` (`param_1`) | `*(float*)(FrameImage+0x10570)` | set from the ambient estimate |
| `width` (`param_2`) | `0xf0` / `0x100` / `0x180` / `0x280` | `FrameImage+0x150` |
| total height (`param_3`) | active + 4 | `FrameImage+0x154` |
| `lut` (`param_4`) | `FrameImage+0x550`, 16 384 floats | output |
| reference band (`param_5`) | `raw + width*(height-4)` | frame buffer |
| `corr`/`refl`/`air`/`humi`/`emiss` (`param_6..param_11`) | `FrameImage+0x10550/54/58/5c/60/64` | outputs |
| `userArea` (`param_12`) | `FrameImage+0x10568` | output (`u16*`) |
| `sensor_mode` (`param_13`) | `FrameImage+0x1056c`, default **`0x82`** | per-unit |
| `fix_mode` (`param_14`) | `FrameImage+0x10574`, default **`400`** (≠ `0x78` → off) | per-unit |

> **Correction (supersedes the earlier "param_14 = distance" claim).** `param_14`
> is **`fix_mode`**, the `mode` argument passed to `GetFix` (`GetFix(amb2,
> fix_mode, width)` at `0x1630`). Only `0x78` makes `GetFix` non-zero; any other
> value (including the `400` default) disables it and `base = userArea[0]`.
> Verified at `0x1620: ldr w0, [sp,#304]` (stack arg 4) → `bl GetFix`.
>
> The **distance** the model actually uses is **not** a parameter at all — it is
> read from the **frame** at `rec+0x112` (u16): `0x14d4: ldrh w0, [x3, x2]`
> with `x2 = rec_base + 0x112`, stored to `*userArea_out` at `0x14d8`, then
> converted to float and compared against `20.0` at `0x167c: fcmpe s9, #20.0`.
> The vendor log's `distance:%d` field therefore prints the **frame value** at
> `rec+0x112`, not `param_14`. In the harness the log reads `distance:419`
> because `0x1A3` (= 419) was written there — **not** a varargs alignment
> artifact as an earlier session speculated. **[V]**

Two corrections to §5.5's pseudo-C, both **[V]**:

* The calibration record does **not** start at `userArea + 6`. There is a **width-dependent record
  base** in between — `0x1e0` (240), `0x200` (256), `0x900` (384), `0xf00` (640). The `p` in §5.5
  is the `rec_base == 0` case only, which never occurs for a real sensor width. Use
  `04-usb-protocol.md` §4.5.2's offset table instead.
* `param_14` is **`fix_mode`**, not distance — see the note above. §5.7 item 1
  ("units of distance") is therefore **moot for `param_14`**; the distance read
  from the frame at `rec+0x112` is the value that participates in the
  `< 20.0` close-range branch. The `400` stored at `FrameImage+0x10574` is a
  `fix_mode` default, not 400 mm. **[V]**

### 5.8.1 `humi` is a 0…1 fraction, not percent **[V]**

`CalcFixRaw` uses `humi` raw: `tau = humi * exp(poly(air))` (`0x0b10:
fmul d0, d1, d0` with `d1 = (double)humi`). There is **no `/100`**. The
atmospheric transmittance `tau_eff = 1.9·e1 − 0.9·e2` is only physical (in
`(0,1]`) when `humi ≤ ~1`. With `humi > 1` (e.g. the `56.39` an earlier session
wrote to match a misread log), `tau_eff` goes **negative**, which flips the sign
of `inv_te = 1/(tau_eff·emiss)` and makes `GetTempEvn` *decreasing* in `s`,
inverting the LUT. A real device stores `~0.5` (50 % RH as a fraction); the
vendor log then prints `humi:0.5…`. Confirmed empirically: `humi=0.5` →
finite, monotonically **increasing** LUT (44.6 → 62.1 °C).

**Minimum viable port:** build the LUT once per parameter change via `thermometryT4Line`, then
convert every frame with `thermometrySearch`. No per-pixel transcendental evaluation is needed.
For mode `1000`/`0x3eb` devices, skip both entirely — the conversion is `raw/64 − 273.15`.
