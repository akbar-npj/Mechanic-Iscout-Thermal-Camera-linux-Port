# 08 — Linux Port Plan

This document turns the findings in `04-usb-protocol.md` and `05-thermometry-algorithm.md` into a
concrete implementation plan. It is a **proposal**, not a verified artifact — the tags indicate
how much of each step rests on established fact.

---

## 8.1 Why this port is short

The single most important finding is that **the vendor did not write their own USB stack**. **[V]**

`libuvc.so` embeds upstream libuvc source paths:

```
jni/libuvc/android/jni/../../src/{init,device,ctrl,stream,frame,diag}.c
```

Upstream libuvc builds natively on Linux against `libusb-1.0`. Everything the vendor added on top
is a thin layer:

* `uvc_diy_communicate` — a **pass-through wrapper around `libusb_control_transfer`** whose only
  logic is `if (ret == 8) ret = 0;`. **[V]**
* `uvc_diy_start_preview` / `uvc_diy_stop_preview` — one-instruction tail-call wrappers. **[V]**
* `uvc_find_device2`, `uvc_init2`, `uvc_already_open` — convenience wrappers. **[V]**
* `uvc_get_device_with_fd` — **Android-only** (the Android USB host API hands the app a file
  descriptor). **Not needed on Linux.** **[V]**

So the transport layer is ~150 lines of glue, not a rewrite.

The second important finding is that **the thermometry is plain C with no JNI and no vendor
framework** — `libthermometry.so` is 13,856 bytes and was decompiled in full. **[V]**

---

## 8.2 Proposed architecture

```
                    ┌──────────────────────────────────────┐
                    │  application (CLI / GUI / library)   │
                    └───────────────┬──────────────────────┘
                                    │
      ┌─────────────────────────────┼──────────────────────────────┐
      │                             │                              │
┌─────▼──────┐              ┌───────▼────────┐            ┌────────▼────────┐
│ thermometry│              │  frame decode  │            │  palette / LUT  │
│  (port of  │              │  raw 14-bit →  │            │  768 B RGB24    │
│libthermome │              │  uint16[]      │            │  index 0 = cold │
│   try.so)  │              └───────┬────────┘            └─────────────────┘
└─────┬──────┘                      │
      │                             │
      └──────────┬──────────────────┘
                 │
        ┌────────▼─────────┐        ┌───────────────────────┐
        │  vendor control  │◄──────►│   libuvc (upstream)   │
        │  uvc_diy_* glue  │        │   + libusb-1.0        │
        └──────────────────┘        └───────────┬───────────┘
                                                │
                                        ┌───────▼────────┐
                                        │  USB device    │
                                        └────────────────┘
```

Three layers, each independently testable:

| Layer | Risk | Depends on |
|---|---|---|
| `libusb` + upstream `libuvc` | **very low** — off-the-shelf, already works on Linux | nothing |
| vendor control glue | **low** — protocol fully recovered | `04-usb-protocol.md` §4.2 |
| frame decode + thermometry | **low** — layout and math both fully recovered | `04-usb-protocol.md` §4.5, `05-…` |

---

## 8.3 Phase 0 — bring-up and identification

**Goal:** prove the device enumerates and that the vendor control channel works.

```bash
# 1. identify the device
lsusb
lsusb -v -d <vid>:<pid> 2>/dev/null | head -100

# 2. confirm it is a UVC device and list its streaming formats
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats-ext

# 3. check permissions
#    the device node must be readable by your user; add a udev rule rather than running as root
```

Add a udev rule (do **not** run the client as root):

```
# /etc/udev/rules.d/99-dyt-thermal.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="<vid>", ATTR{idProduct}=="<pid>", MODE="0666", GROUP="plugdev"
```

The VID/PID list from `01-target-and-hardware.md` §1.3 is a **candidate list from the APK's
`device_filter.xml`**, but it is no longer just a filter: `UVCCamera::connect` maps each pair to a
**device mode** which selects the entire thermometry path (`04-usb-protocol.md` §4.10). `lsusb`
tells you which row applies. **[V]**/**[?]**

> **Expect the device to withhold frames until the serial handshake completes** if the mode is
> `0x44c` (i.e. VID/PID `1514:0001`). See `04-usb-protocol.md` §4.10 — this is the single most
> likely cause of a "device works but no thermal data" dead end.

> **Correction (2026-09-24, live device).** This phase is done for the unit in hand, and both
> expectations above were wrong for it. The device is **`0bda:5840` → mode `1000`**, so no serial
> handshake is involved; and mode `1000` does **not** emit real frames on its own — it streams a
> flat `0x8000` placeholder until `setTinyCOutputADValue` is sent *after* streaming starts. See
> `04-usb-protocol.md` §4.10 for the full sequence and the `0x0e` latch to avoid.

**Deliverable:** a shell transcript showing the device's VID/PID, interfaces, endpoints and
supported UVC formats — plus the `bFormatIndex` of the thermal stream, which is the only
frame-related fact still outstanding.

> **Correction (2026-09-24).** The `bFormatIndex` is no longer outstanding: it is **`1`**, the only
> format on interface 1 (UNCOMPRESSED YUY2, 16 bpp, 256×192 @ 25 fps). See
> `04-usb-protocol.md` §4.5.6.

---

## 8.4 Phase 1 — control channel

**Goal:** implement and exercise the vendor control primitive.

Implement exactly the sketch in `04-usb-protocol.md` §4.6:

```c
int uvc_diy_communicate(uvc_device_handle_t *devh,
                        uint8_t bmRequestType, uint8_t bRequest,
                        uint16_t wValue, uint16_t wIndex,
                        uint8_t *data, uint16_t wLength, unsigned timeout);
```

Two behaviours must be reproduced **exactly**, or timing-sensitive device states will differ:

1. **Return-value normalisation:** the vendor maps `ret == 8 → 0`. For 1-, 2-, 15- and 28-byte
   transfers the raw libusb byte count is returned. **[V]**
2. **Timeout is always 1000 ms** at every observed call site. **[V]**

Then implement the canonical transaction (`04-usb-protocol.md` §4.2):

```
OUT  bmRequestType=0x41 bRequest=0x45 wValue=0x0078 wIndex=0x1d00  <8-byte command>
poll bmRequestType=0xC1 bRequest=0x44 wValue=0x0078 wIndex=0x0200  <1-byte status>
     until (status & 0x01)==0 && (status & 0x02)==0, max 1000 iterations
     abort if (status & 0xfc) != 0
IN   bmRequestType=0xC1 bRequest=0x44 wValue=0x0078 wIndex=0x1d08  <15-byte result>
```

**First safe probe:** use a *read-only* order. `UVCPreviewIR::getTinyCRobotSn`,
`getTinyCUserSn` and `getMachineSetting` are all reads. Reading the factory serial number is a
good smoke test because it has a known, checkable answer format (`DYT…`, see
`06-asset-and-file-formats.md` §3.3). **[I]**

> **Do not call `setTinySaveCameraParams` or `setMachineSetting` until the whole read path is
> understood.** These write calibration to the device. A wrong write could invalidate the unit's
> factory calibration, which is not recoverable from the artifacts we have. **[I]**

**Blocker:** the complete opcode table is not yet decoded. It is mechanically recoverable by
enumerating the order-construction sites — see `09-…` §2.

---

## 8.5 Phase 2 — frame capture and decoding

**Goal:** get raw thermal frames into a `uint16[]`, and temperatures out of them.

**This phase is no longer unresolved.** The layout is fully specified in
`04-usb-protocol.md` §4.5. Recap:

* The frame is a **flat, row-major `uint16` array, `stride == width`**, of
  `width × (active_height + 4)` samples. Raw samples are 14-bit, valid `0 … 16383`. **[V]**
* The **last 4 rows are a reference/shutter band** (the `userArea`), not image data. They begin at
  byte offset `width * active_height * 2` and carry the per-unit calibration record
  (`04-usb-protocol.md` §4.5.2).
* The thermal image is `width × (height − 4)` — e.g. **256 × 192** for a 256-wide sensor, which
  matches the resolution in the user manual.
* The vendor's output is a `float` array of `width*(height-4) + 10`: a 10-element header
  (ambient, max x/y/T, min x/y/T, 3 reserved) followed by the row-major image.
* Per pixel: `out[10+k] = lut[raw[k]] + corr`, where `lut` is the 16 384-entry curve built by
  `thermometryT4Line` and `corr` is read from the calibration record.

### Decoder skeleton

```c
#define REF_ROWS 4

typedef struct {
    int       width, total_height;   /* total_height = active + 4 */
    int       rec_base;              /* 0x1e0 | 0x200 | 0x900 | 0xf00 */
    uint16_t *raw;                   /* width * total_height samples */
} frame_t;

/* The reference band is the last REF_ROWS rows of the frame buffer. */
static inline uint8_t *user_area(const frame_t *f)
{
    return (uint8_t *)f->raw + (size_t)f->width * (f->total_height - REF_ROWS) * 2;
}

/* Calibration record inside the reference band. */
static inline uint8_t *cal_record(const frame_t *f)
{
    return user_area(f) + f->rec_base;
}

/* Returns 0 on success, -1 if the frame is saturated/corrupt. */
static int convert(const frame_t *f, const float *lut, float *out /* w*(h-4)+10 */)
{
    const int n = f->width * (f->total_height - REF_ROWS);

    /* per-frame offset, read from the record (see note below) */
    float corr = *(const float *)(cal_record(f) + 0xFE);

    for (int k = 0; k < n; k++) {
        if (f->raw[k] >= 0x4000)     /* device-identical validity gate */
            return -1;
        out[10 + k] = lut[f->raw[k]] + corr;
    }
    return 0;
}
```

> **`corr` is verified.** It is read from the calibration record at `+0xFE` relative to
> `rec_base`. This was confirmed symbolically for all four sensor widths and **empirically** by
> the byte-identical port (§8.6.1): the vendor's width-dependent bit-twiddled index in
> `thermometrySearch` resolves to exactly `width*(h-4)*2 + rec_base + 0xFE` bytes from `raw`.
> The earlier draft's "two code paths disagree for 384/640" caveat was a misreading of the
> inlined expression — they agree. **[V]**

### Two things the port must not omit

1. **The 14-bit validity gate.** Any `raw[k] >= 0x4000` means a saturated or corrupt frame; the
   vendor aborts the whole frame and logs `"thermometrySearch err : 16383"`. Match that behaviour
   rather than clamping to range.
2. **The auto-shutter (FFC) trigger** (`04-usb-protocol.md` §4.5.5). Watch the reference band's
   second sample; when it drifts by ≥ 15 counts from the last shutter, issue
   `uvc_set_zoom_abs(cam, 0xffff8000)`. Skipping this produces visible drift and banding within
   minutes of warm-up — it is not cosmetic.

### If the device turns out to be mode `1000` / `0x3eb`

There is no reference band and no LUT. The conversion is `raw/64 − 273.15` over `width × height`
samples (`FrameImage::adValueArray2FloatTempArray`). That is the easiest possible bring-up path
and is worth trying first if `lsusb` reports a `0BDA:*` device. **[V]**

### Still needs the device

Only one frame-related fact remains: **which `bFormatIndex` carries the thermal stream** (and
therefore whether visible and thermal arrive as two interfaces or two formats on one). `lsusb -v`
answers it. Everything needed to *parse* the frame is already known.

---

## 8.6 Phase 3 — thermometry

**Goal:** raw sample → °C.

Directly port `thermometryT` from `05-thermometry-algorithm.md` §5.5. The document contains
faithful pseudo-C for the whole computation and a complete constant table (§5.6). There is no
guesswork here — the module is plain C and was fully decompiled. **[V]**

Key implementation notes:

* Build a **16,384-entry `float` LUT** once per parameter change; index it per pixel. Do not
  evaluate transcendentals per pixel. **[V]**
* The six parameters come from the frame's user area. **[V]**
* Reject measurement points where any sampled raw value is `>= 0x4000`. **[V]**
* The constants `-0.9` and `1.9` at `0x100c80`/`0x100c88` are **doubles**, as are the
  `distanceFix` constants. Reading them as floats produces nonsense (this mistake was made and
  corrected during the analysis). **[V]**

**Validate against the vendor:** the Windows app displays absolute temperatures. Point the camera
at a blackbody or a known reference, record what the Windows app shows, and compare with the port.
A constant offset indicates a calibration-table problem (Phase 4).

### 8.6.1 Phase 3 — STATUS: PORT WRITTEN AND BYTE-VERIFIED **[V]**

The portable C reimplementation is **complete and verified byte-for-byte** against the vendor
`libthermometry.so` for **all four sensor widths** (240 / 256 / 384 / 640), in both `fix_mode`
configurations. This was done statically — no hardware was required.

**Artifacts (all under `linux-port/tools/thermometry_diff/`):**

| File | Role |
|------|------|
| `port.c` | the Linux port — from-scratch reimplementation of `thermometryT4Line` + `thermometrySearch` |
| `harness.c` | drives the *real* vendor `libthermometry.so` to produce byte-exact ground truth |
| `run.sh` | builds the bionic→glibc shim libs, preps the vendor `.so`, builds the harness, runs it |
| `diff.py` | byte-diffs `port_*.bin` against the frozen vendor `in_lut_in.bin` / `out_image.bin`; exit 0 on PASS, non-zero on FAIL (CI-usable) |
| `out/<width>/` and `out/<width>_fixon/` | frozen ground truth + port output, `fix_mode=400` (off) and `0x78` (on), for each width |

**Reproduce (256 case; the others differ only in the harness args `width height rec_base`):**

```bash
cd linux-port/tools/thermometry_diff
./run.sh                         # build shims + harness, regenerate vendor ground truth (256)
cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -o build/port port.c -lm
./build/port out/256 && ./build/port out/256_fixon
./diff.py                        # → OVERALL: PASS (all 8 cases)
```

**Verified results (SHA256 of each file; vendor `in_lut_in.bin`/`out_image.bin` ==
port `port_lut.bin`/`port_out.bin`):**

| Width | fix_mode | LUT SHA256 | image SHA256 |
|-------|----------|------------|--------------|
| 240 (GetFix off) | `400`  | `4a953a27e4eb0698189962e9195b6129a411006f05cb3a178d656ead17da9eb5` | `1368b029ea22a180face287efcb6b787e03b614814b6cd8925106c9e8440c6ea` |
| 240 (GetFix on)  | `0x78` | `e17105e516bb3aa9de35ee40b81bd95900bed46d392509f266a0c50161c1c1a4` | `275e8bb5f0a6dfc25d0edb9e50906af0b6764427c3943fe5bc4fa9544f9c647b` |
| 256 (GetFix off) | `400`  | `eb70ef58c4c7f36df03bf853493a5b3dced273f5f157bf046147457f318bfc17` | `f7d16b1b221cc12aeef4acfe37fbbbf4b9ce3f5f8aa7edb19ef7bebbd402f191` |
| 256 (GetFix on)  | `0x78` | `1441fd2afac362495dd39623fe072896018c8ab995b1b392b58ac8b3b15785f4` | `528b5ba63feb8b15375a67f3700f25d37b4953a716c348253ccdfb599207bbd1` |
| 384 (GetFix off) | `400`  | `4a953a27e4eb0698189962e9195b6129a411006f05cb3a178d656ead17da9eb5` | `0884a541c402f9cf3b8bffb586553b013b5266238146360bc5fae312aca43864` |
| 384 (GetFix on)  | `0x78` | `e17105e516bb3aa9de35ee40b81bd95900bed46d392509f266a0c50161c1c1a4` | `3b0bc744040b32b3b958f0c8def8c5953f8bea8692bb7bc8342dff392fc678ed` |
| 640 (GetFix off) | `400`  | `107435f88e747d734af83106d53dde9200a680c6a101c2040e35f93db823893d` | `8ad844e73b137cea5de1ece68d6e5c058d67c4b4da586cc15958a04ad770ccc2` |
| 640 (GetFix on)  | `0x78` | `ea7466e9e76b532a05b5b8fe17243997f9cecb9f3ff9247da087fa0d839273ac` | `c61491bb1dfb2a7a1f4c1d49565196f7746006fce8134e34f1e3503237b33897` |

All four sensor widths (240 / 256 / 384 / 640) are byte-verified in both `fix_mode`
configurations. For each width, both the 16,384-entry `float` LUT and the full output image
(`10`-float header + `width×(height-4)` pixels) match the vendor library **byte-for-byte**,
including NaN entries (the port's glibc `sqrt` produces the same canonical qNaN as the vendor's
ARM `fsqrt`). **[V]**

**Reproducing the ground truth.** The exact commands, verified 2026-09-20 to regenerate all eight
sets byte-identically:

```bash
cd linux-port/tools/thermometry_diff
# fix_mode=400 (GetFix off).  256 uses the default c=d=0; the other widths need
# DYT_C=0.0001 or you get different files — see the coverage note below.
./run.sh                    out/256        256 196 0x200
DYT_C=0.0001 ./run.sh       out/240        240 184 0x1e0
DYT_C=0.0001 ./run.sh       out/384        384 292 0x900
DYT_C=0.0001 ./run.sh       out/640        640 480 0xf00
# fix_mode=0x78 (GetFix on)
DYT_FIXMODE=0x78 ./run.sh                    out/256_fixon 256 196 0x200
DYT_C=0.0001 DYT_FIXMODE=0x78 ./run.sh       out/240_fixon 240 184 0x1e0
DYT_C=0.0001 DYT_FIXMODE=0x78 ./run.sh       out/384_fixon 384 292 0x900
DYT_C=0.0001 DYT_FIXMODE=0x78 ./run.sh       out/640_fixon 640 480 0xf00
```

> **The `DYT_C` override is not optional.** Omitting it for 240/384/640 changes 4 bytes in the
> reference band's calibration record (`rec+0x0f`, inside the `c` coefficient) and therefore the
> whole LUT and image. Passing a temp directory as the output keeps the committed ground truth
> intact if you only want to verify.

The harness is otherwise environment-driven: `DYT_LO`, `DYT_AMBADV`, `DYT_TCODE`, `DYT_A`…`DYT_E`,
`DYT_CORR`, `DYT_REFL`, `DYT_AIR`, `DYT_HUMI`, `DYT_EMISS`, `DYT_UA0/1` and `DYT_FIXMODE` all
override their defaults. Any of them being set in your shell will silently change the output, so
check `env | grep DYT_` before regenerating and expecting a match.

> **Note on the 240/384 LUT collision.** Widths 240 and 384 share identical LUT SHA256 hashes
> (e.g. `4a953a27…` for `fix_mode=400`) because they share the same `ambadv=0x1E78` and
> `scale=36.0`, and the LUT does not depend on width directly — only on `amb`, the calibration
> coefficients, and the fix/`sensor_mode`. The *images* differ (different pixel counts and raw
> values). Width 640 (`ambadv=0x1AD3`, `scale=33.8`) produces a distinct LUT, independently
> exercising its `ambadv` branch. **[V]**

> **Note on NaNs.** The 240/384/640 cases with `DYT_C=0.0001` produce a non-physical `amb`
> (because the harness uses the 256-sensor's default `DYT_AMBADV=0x21A9` for all widths) and
> therefore many LUT entries are `sqrt(negative) = NaN`. These NaNs match the vendor's
> byte-for-byte, which is the point — the port's transcendental edge cases are correct. A
> physically-meaningful `amb` is restored by setting `DYT_AMBADV` to the width's documented value
> (`0x1E78` for 240/384, `0x1AD3` for 640); this was not done for the frozen cases because the
> byte-identity proof does not require it. **[V]**

**Precision discipline that made it byte-identical** (derived from disassembly, documented in
`port.c`'s header comment and `05-thermometry-algorithm.md` §5.6/§5.8):

* `-ffp-contract=off` + explicit `fmaf()` at every NEON `fmadd`/`fmsub`/`fnmadd`/`fnmsub` site.
  C `a*b+c` is two roundings; fused FMA is one. Without this the port drifts at the last bit.
* Single-precision (`float`) between double `pow()`/`exp()` calls, matching the `fcvt` promotions
  and demotions in the disassembly.
* glibc libm transcendentals on both sides — the harness's shim forwards the vendor's `pow`/`exp`/
  `sqrt` to the *same* glibc libm, so transcendental results are bit-identical by construction.

**Signature/semantics corrections pinned down during the port** (these supersede earlier
readings; see `05-thermometry-algorithm.md` §5.8 for the full evidence):

* `param_14` (stack arg 4 of `thermometryT4Line`) is **`fix_mode`**, not `distance`. Only `0x78`
  makes `GetFix` non-zero. **[V]**
* The distance the model uses is read from the **frame** at `rec+0x112` (u16), not from a
  parameter. **[V]**
* `humi` is a **0…1 fraction** (relative humidity), not a percent. `CalcFixRaw` uses it raw:
  `tau = humi * exp(poly(air))`. With `humi > 1`, `tau_eff = 1.9·e1 − 0.9·e2` goes negative,
  flipping `GetTempEvn`'s sign and inverting the LUT. **[V]**
* `thermometrySearch` returns **`void`** (verified at `0x1d38..0x1d44`: no `x0` set before `ret`).
  **[V]**
* The search "base value" added to every LUT lookup is **`corr`** — the float at `rec+0xFE` —
  not a LUT entry. The vendor's width-dependent bit-twiddled index resolves symbolically to
  exactly `width*(h-4)*2 + rec_base + 0xFE` bytes from `raw` for all heights. **[V]**
* The 10-float image header layout (`out[0]=corr+lut[r[12]]`, `out[1]=(float)r[2]`, …,
  `out[9]=corr+lut[r[8]]` with `out[7]==out[9]` duplicated) was reverse-engineered from disasm
  `0x1c20..0x1d1c` and is reproduced exactly. **[V]**

**Coverage:** all four sensor widths the device ships with (240 / 256 / 384 / 640) are
byte-verified, in both `fix_mode` configurations. The 256 cases use the default coefficients
(`c=d=0`, `e=0.0001`); the 240/384/640 cases use `DYT_C=0.0001` so that `q2` depends on `amb`,
exercising the width-specific `ambadv`/`scale` branch (which the `c=d=0` default would leave
untested). Width 640 independently exercises its distinct `ambadv=0x1AD3` / `scale=33.8` branch.
**[V]**

**What remains for thermometry:** nothing static. The port is verified for every sensor width the
device ships with. The only remaining thermometry work is live validation against a real blackbody
once the hardware arrives (§8.6). **[V]**

---

## 8.7 Phase 4 — calibration (only if accuracy demands it)

The Windows build ships `tau_H.bin`, `tau_L.bin`, `MILI6_*.bin` and `block_lut.dat`; the Android
build does not. **[V]** This strongly suggests the Windows build applies software calibration
that the Android build does not. **[I]**

**The tables are decoded** — see `10-calibration-tables.md`. `linux-port/src/calib.{h,c}` loads
them and reproduces the vendor's index selection, row-0 degenerate rule, bilinear interpolation,
`+0.5` rounding and Q14 scaling; `calib_test` pins all of it. The blobs themselves are in
`linux-port/calib/`. `block_lut.dat` turned out not to be read by any native thermometry code, so
it is not needed.

What remains is **wiring**, not decoding: `calib.c` is not yet called from `capture.c`, because
the vendor applies the correction inside `enhance_distance_temp_correct` using an
`(ε, humidity, distance, atmospheric temperature)` parameter block that the Linux port has no
source for yet — the Windows app gets those from the user, the Android app from
`getTinyCParams`. Decide that policy before wiring it in.

If Phase 3 produces temperatures that are self-consistent but absolutely offset, the calibration
tables are the likely cause. They are now available to test that hypothesis.

**Do not start here.** Get a working, self-consistent reader first; a relative-temperature thermal
camera is already useful, and chasing calibration before the frame layout is known is wasted work.

---

## 8.8 Phase 5 — output

| Feature | Source |
|---|---|
| Palette rendering | 768-byte RGB24 LUTs, index 0 = coldest (`06-…` §1) |
| Palette assets | copy `apk/assets/{1..7}.dat`; the Windows `lut_*.dat` are byte-identical where they overlap (`06-…` §1.3) |
| DYT file save/load | container format in `06-…` §2 |
| Alarm sounds | the 8 WAV files in the Windows installer; plain RIFF |
| Reporting | entirely new code — no vendor format to match |

---

## 8.9 Repository layout

As built (git `main`, initialised 2026-09-20). An earlier version of this section
proposed a flat `dyt-thermal/` tree; the port is instead self-contained under
`linux-port/`, so it builds and tests on its own.

```
Thermal Camera/
├── RE Docs/                this document set
├── RE Workspace/           generated, disposable (see 02); only the scripts are tracked
├── linux-port/
│   ├── Makefile            `make` builds; `make check` is the regression gate
│   ├── src/
│   │   ├── control.c       uvc_diy_communicate + the transaction state machine
│   │   ├── frame.c         frame acquisition + user-area parsing
│   │   ├── thermometry.c   port of libthermometry.so
│   │   ├── calib.c         tau_*.bin / MILI6_*.bin reader (not wired in; see 10 §4.5)
│   │   ├── capture.c       libuvc streaming layer
│   │   ├── serial.c        serial-number decode (DecryptSNE)
│   │   └── dumpframe.c     Phase 2: run the pipeline over a frozen frame
│   ├── calib/              the four extracted calibration blobs
│   ├── third_party/libuvc/ vendored v0.0.8, compiled from source — see below
│   └── tools/
│       ├── probe.c         Phase 0/1 smoke test: enumerate + read serial
│       ├── capture_demo.c  live UVC capture demo
│       ├── taucmp.c        tau-model comparison diagnostic (see 10 §8.3)
│       └── thermometry_diff/
│           ├── harness.c   runs the vendor libthermometry.so for ground truth
│           ├── port.c      runs the port, for byte-comparison
│           ├── diff.py     the comparison
│           └── out/<cfg>/  frozen vendor ground truth, one dir per configuration
└── iScout … v3.0.6+windows/ vendor originals (not tracked)
```

Two deviations from what this section originally proposed:

* **libuvc is vendored as source files, not a git submodule.** The build needs
  `include/libuvc/libuvc_config.h`, which is hand-written here to replace the header
  CMake would generate and therefore does not exist upstream — a gitlink would drop it
  and break the build. The vendored sources are compiled directly by `linux-port/Makefile`,
  with JPEG disabled.
* **The opcode table lives in `control.c`**, not a separate `orders.c` as sketched above.

Not yet written: `palette.c` (768-byte RGB24 LUT loader) and `dytfile.c` (DYT container
read/write) — Phase 5, see §8.8.

---

## 8.10 Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| ~~Frame layout unknown~~ | **resolved** | fully specified statically — `04-usb-protocol.md` §4.5 |
| Thermal `bFormatIndex` unknown | delays first live frame | `lsusb -v` (§8.3) — the only frame-level unknown left |
| **No thermal frames despite a working device** | looks like a protocol failure | the serial-number handshake gates frame delivery in mode `0x44c` (`04-usb-protocol.md` §4.10). Complete the SN check before debugging anything else. |
| Drift / banding after minutes | silent accuracy loss | implement the auto-shutter trigger (§8.5, `04-…` §4.5.5) |
| Opcode *semantics* partly inferred | blocks stream start on unfamiliar modes | enumerate order-construction sites (`09-…` §2) |
| Writing calibration bricks the unit | **irreversible** | read-only probes only until the write path is fully understood (§8.4) |
| Wrong VID/PID assumption | wastes time | `lsusb` first (§8.3) |
| Absolute accuracy off | cosmetic for many uses | Phase 4, or accept relative temperatures |
| Vendor firmware update changes protocol | future breakage | pin the known-good firmware/APK version; hash the relevant artifacts before/after |

---

## 8.11 What is *not* needed

For clarity, these are **not** required and should not be attempted:

* **Reproducing the serial-number licence check** (`06-…` §3.2). It gates the *vendor's* app; a
  client we write has no reason to implement it.
* **The Windows binaries.** `libiruvc.dll` is WinUSB-based and not portable; the Android
  `libuvc.so` is the reusable one. Use the Windows payload only for calibration data and
  cross-checking (`07-…` §5).
* **The WiX Burn bootstrapper / VC++ redistributables** — Microsoft packaging, unrelated to the
  device (`07-…` §1).
* **`libMNN.so` / `libMNN_Express.so` / `libmnnmodel.so`** — an on-device neural-network runtime
  shipped with the Android app. Its role is not established (`09-…` §6) but it is almost
  certainly *not* required for basic thermal capture.
