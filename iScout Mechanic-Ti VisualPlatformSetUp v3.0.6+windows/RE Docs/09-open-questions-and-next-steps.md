# 09 — Open Questions and Next Steps

Everything still unproven, ordered by how much it blocks the Linux port. Tags follow
`README.md` §3.

**Status as of 2026-09-20 (fourth session).** The port is no longer blocked on anything that can be
determined statically. Closed: §1 (frame layout) and §3 (calibration table formats). The
thermometry port is byte-verified against the vendor library, and the bring-up, control, frame and
capture layers are written and covered by `make check`. §9 is new — it records a *negative* result
about the calibration tables that removed them from the critical path.

**Remaining open items are all low priority and none blocks the port.** §4 is the only one that a
single command answers. The project is otherwise waiting on hardware; see the prioritised action
list at the end.

| § | Subject | Status |
|---|---|---|
| 1 | Thermal frame buffer layout | **closed [V]** |
| 2 | Opcode semantics | partly inferred **[I]** |
| 3 | Calibration file formats | **closed [V]** — but see §9 |
| 4 | Which VID/PID is this unit? | one command left **[V]** |
| 5 | AES key vs IV role | **[I]** |
| 6 | Unidentified artifacts | **[?]** |
| 7 | Thermometry sub-questions | **[?]** |
| 8 | DYT container field semantics | **[?]** |
| 9 | Which transmittance model is correct? | **[?]** — new, does not block |

---

## §1 — Thermal frame buffer layout — **RESOLVED [V]**

**Status: closed.** This was the only hard blocker, and it was resolved **statically** — no
hardware, no USB capture. Full detail in `04-usb-protocol.md` §4.5. Summary:

* The raw frame is a **flat `uint16` array, row-major, `stride == width`**, of
  `width × (active_height + 4)` samples.
* The **last 4 rows are a reference / shutter band** (the code calls it the `userArea`) and are
  **not image data**. They begin at byte offset `width * active_height * 2`.
* A per-unit **calibration record** is embedded in that band at a width-dependent base
  (`0x1e0` / `0x200` / `0x900` / `0xf00` for widths 240 / 256 / 384 / 640), with fixed offsets
  from there — including the 32-byte serial key blob at `+0x40` and the encrypted serial at
  `+0x162`. See `04-usb-protocol.md` §4.5.2.
* The output image is **`width × (height − 4)`**, delivered as a `float` array of
  `width*(height-4) + 10`: a 10-element header (ambient, max x/y/T, min x/y/T, 3 reserved)
  followed by the row-major image.
* Per pixel: `out[10+k] = lut[raw[k]] + corr`, where `lut` is the 16 384-entry `float` curve built
  by `thermometryT4Line` and `corr` is read from the reference band.
* Validity gate: **any `raw[k] >= 0x4000` aborts the frame** (`"thermometrySearch err : 16383"`).
* An **auto-shutter (FFC) trigger** watches `raw[width*(height-4) + 1]` and issues
  `uvc_set_zoom_abs(cam, 0xffff8000)` when it drifts by ≥ 15 counts. A port that omits this will
  drift and band within minutes. See `04-usb-protocol.md` §4.5.5.

> **Correction to an earlier draft of this section.** It previously asserted that there was *no*
> user area inside the pixel plane, and that the thermometry parameters therefore lived in one of
> the *other* allocated buffers. **That was wrong.** The parameters *are* inside the frame buffer —
> in the last 4 rows. This is exactly why `thermometryT4Line`'s offsets (`0x200 … 0x314` for
> `w = 0x100`) appeared to run "past" a `w*2`-byte row: they run past the **image**, into the
> appended reference band. The misreading came from treating the allocated height as the *active*
> height.

### Residual sub-questions **[?]** — all need `lsusb -v`

* The UVC frame format GUID / `bFormatIndex` for the thermal stream.
* How the visible and thermal streams are demultiplexed at the UVC level.
* The meaning of the frame struct's `+0x20` format tag (`3` = raw16 frame, `8` = RGBA preview).

### How it was resolved — reproducible static chain **[V]**

1. `libthermometry.so` `thermometrySearch @ 0x101bf0` — computes `n = (height - 4) * width` and
   runs the image loop over exactly `n` samples. Proves the `-4`.
2. `libuvc.so` `uvc_allocate_ini_frame @ 0x10ab24` — `0x36C00 == 384 × 292 × 2` with
   `292 == 288 + 4`. Proves the extra rows are physically in the buffer.
3. `libUVCCamera.so` `do_preview` (~line 26706) — the `userArea` for a 256-wide frame is at
   `raw + 0x18000 == raw + 256 × 192 × 2`.
4. `libUVCCamera.so` `FrameImage::do_temperature_callback @ 0x181ff8` — the `jfloatArray` is
   `width*(height-4) + 10`.
5. **Cross-check:** the record offsets `+0x40`, `+0xfe`, `+0x112`, `+0x162` land on the same
   fields in both the 256 and 384 layouts, whose record bases differ (`0x200` vs `0x900`).

The lesson for future sessions: **the two `usbmon`-based plans in the previous draft of this
section were unnecessary.** When a blocker looks hardware-only, first check whether the vendor
library already contains the arithmetic — here three separate functions did.

---

## §2 — Opcode semantics **[I]**

**Priority: medium.** The *table* is recovered (`04-usb-protocol.md` §4.2); the *meanings* are
partly inferred.

| opcode | inferred meaning | confidence |
|---|---|---|
| `0f c1 …` | start thermal stream | high — the function is literally `tinyStartStream` |
| `0a 01 …` | output raw AD values | high — function is `setTinyCOutputADValue` |
| `14 85 …` | read parameter block | high — `getTinyCParams` |
| `14 c5 …` | write parameter block | high — `sendTinyCParamsModification` |
| `14 83 …` / `14 c3 …` | unknown pair used by `do_tinyC_order` case 7 | **low** |
| `0d 8b …` / `0d c1 …` | unknown; `0d c1` also used by `getTinyCUserData` | **low** |
| `00 00 00 00 00 00 00 02` | not a command — a receive-buffer pre-fill | high |
| 9-byte `00 00 01 00 01 80 19 00 02` | second stream-start step, written to `0x1d08` | high (function name) |

**Next step:** the parameter block layout. Read the 15-byte and 28-byte results returned by
`getTinyCParams` / `getTinyCUserSnCoefficient` on real hardware and match them against the
thermometry parameter list in `05-thermometry-algorithm.md` §5.3. That single read would pin the
meaning of most of the block.

---

## §3 — Calibration file formats — **RESOLVED [V]** (see `10-calibration-tables.md`)

**Priority: now low.** The formats are decoded, but the tables turned out *not* to be on the port's
critical path (see §9), so this no longer gates absolute accuracy.

Files: `tau_H.bin`, `tau_L.bin`, `MILI6_H_500.bin`, `MILI6_H_91.bin`, `MILI6_L_500.bin`,
`MILI6_L_91.bin`, `block_lut.dat`. All Windows-only. See `06-asset-and-file-formats.md` §4.

**Resolved 2026-09-20.** Full write-up in `10-calibration-tables.md`. The short version:

* `tau_H.bin` / `tau_L.bin` = **7168 bytes** = `uint16[56][64]`, row-major,
  column = distance band, value = **atmospheric transmittance in Q14** (16384 = 1.0). Loaded by
  `Tiny1CDll.dll!FUN_10011000` and consumed by `read_tau_with_target_temp_and_dist` with bilinear
  interpolation and `+0.5` rounding. **[V]**
  > The **row axis is now in doubt.** `Tiny1CDll` reads it as target temperature, but the shipped
  > data has only 8–10 distinct rows and is periodic with period 14 — the environment-temperature ×
  > humidity decomposition that `read_tau` uses. Reader and data disagree. See §9 and
  > `10-calibration-tables.md` §3.4.
* `MILI6_*.bin` = **7424 bytes** = a 256-byte header (`0xFFFFFFFF` magic at +4, version in the
  header's high 16 bits) followed by the same `uint16[56][64]` table. Loaded by
  `DcontrolDll.dll!FUN_10002fb0` (lens mode 1 → `_91`, mode 2 → `_500`) and consumed by
  `libirtemp.dll!enhance_distance_temp_correct`, which also supports 42×88 and 45×88 layouts for
  table versions ≥ 0x40. **[V]**
* Axes (identical in both families): target temperature 248.15 K … 1623.15 K in 25 K steps (56
  entries); distance 0.25 m … 50 m (64 entries), extended to 1000 m (88 entries) in the larger
  versions. **[V]**
* The four blocks are `blk162`/`blk163` (tau) and `blk134`/`blk135` (MILI6); the H/L assignment
  within each pair is documented as `[I]` in `10-calibration-tables.md` §5. **[V]** / **[I]**
* `block_lut.dat` is referenced **only** by the .NET layer (`CA30D.exe`) and is not read by any
  native thermometry code, so it is not needed for the port. **[V]** Its format stays **[?]**.

Correction to an earlier note in this document: `blk162`/`blk163` are **not** float arrays.
7168 bytes is 3584 `uint16`, and the values are `16384` (= 1.0) decaying to `13696`/`11401` — a
Q14 transmittance table, not IEEE floats. **[V]**

Extraction is reproducible:
`python3 tools/extract_calib.py exe/nsis_blocks ../linux-port/calib` (in `RE Workspace/`).

**Still open (all low priority — none blocks the port):**

* the `91` vs `500` lens variant — the shipped blobs do not encode which they are;
* the NSIS file-record table needed to map declared names to blocks — not required, since
  identification was done by structure instead;
* `hash.txt` — declared at the install root but absent from the payload, so it is generated at
  install time;
* the `DAT_1000dfc0` EMS sub-table (`double[86][20]` in `libirtemp.dll`'s `.data`) — extractable, but
  only consulted for table versions ≥ `0x101`, which the shipped tables are not;
* **which τ model is correct for this unit** — see **§9**.

**Cross-check available:** `05-thermometry-algorithm.md` §5.7 item 4 notes that the five
calibration coefficients `a…e` arrive in the frame user area. If they can be correlated with these
tables, both questions are answered at once.

**Superseded note (2026-09-20):** an earlier revision of this section implied the tables were the
difference between a relative and an absolute thermal camera. That framing was wrong — see §9. The
Android build ships no table at all and computes τ in closed form, so the tables are **not** on the
port's critical path.

---

## §4 — Which VID/PID is this unit? — **mostly RESOLVED [V]**, one command left

**Priority: high but trivial to answer.** The five VID/PID pairs in `device_filter.xml` are no
longer just a filter list — `UVCCamera::connect` (`libUVCCamera.so @ 0x15c28c`) **maps each pair to
a device mode**, and the mode determines the entire thermometry path. The full table is in
`04-usb-protocol.md` §4.10 and `01-target-and-hardware.md` §1.3.

What remains is only *which row applies to this unit*:

```bash
lsusb
```

**The 256×192 unit described in the manuals is almost certainly `1514:0001` → mode `0x44C`** (the
radiometric path — it is the only one with a dedicated 256-wide calibration table and a
`256 × 192 × 2` reference-band offset). **[I]**

> **This matters more than it looks.** Mode `0x44c` also gates the **serial-number handshake**:
> `isVerifySN` must succeed against the `configs_maintenanceguy.txt` allow-list before the device
> emits thermal frames. If this unit is `1514:0001`, the Linux port must complete that handshake
> first, or it will appear to receive nothing. See `04-usb-protocol.md` §4.10.

Also worth noting: `device_filter.xml` lists `31DA:5846` but `connect` tests `VID 0x5846 / PID
0x31DA` — the transpose. Four of five entries match; this one does not. **[V]**

---

## §5 — AES key vs IV role **[I]**

**Priority: low** — a licence check we do not need to reproduce.

`AES::AES()` stores two NUL-padded 16-byte values, `"dyt1101c"` and `"dyt0526cdyt0526c"`.
Both key/IV assignments produce plausible serial numbers:

| key | IV | decrypted serials |
|---|---|---|
| `dyt1101c` | `dyt1101c` | `DYTEPK78`, `DYCRPK78`, `DYCRPK79` |
| `dyt1101c` | `dyt0526cdyt0526c` | `DYTDTI08`, `DYCSTI08`, `DYCSTI09` |

The key is corroborated by the differential test in `06-asset-and-file-formats.md` §3.3; only the
IV assignment is ambiguous. **Next step:** read `AES::DecryptionAES` and the key-schedule setup in
`libDYTJpegAes.so` to see which field is fed to the CBC IV.

---

## §6 — Unidentified artifacts **[?]**

**Priority: low.**

| Artifact | Question |
|---|---|
| `libMNN.so`, `libMNN_Express.so`, `libmnnmodel.so` | an on-device neural-network runtime. What model, and what is it used for? Almost certainly irrelevant to basic capture. |
| `M1.exe`, `M2.exe` | shipped in the Windows installer; purpose unknown |
| `blk205` (23,618 bytes, non-PE) | unidentified; sits next to the NSIS uninstaller (`blk206`) |
| `blk165`, `blk168` (PDB) | `[I]` almost certainly `ThermalAnalysis.pdb` / `ThermalAnalysisSystem.pdb` — the only two PDB blocks and the only two `.pdb` names in the file list — but not byte-confirmed |
| `libPhotoNativeHelper.so`, `libsimplePictureProcessing.so` | Android-only helpers; roles not examined |
| `6.dat` palette | ships on Android but has no Windows equivalent |
| `7.dat` palette | ships on Android but is absent from `DYConstants.paletteArrays` |

---

## §7 — Thermometry sub-questions **[?]**

**Priority: low** — carried over from `05-thermometry-algorithm.md` §5.7. Items 4 and 5 there are
now **resolved** (see `05-thermometry-algorithm.md` §5.8) and have been removed.

1. Units of `distance`. The default is `400` (`FrameImage+0x10574`), the `*3` scaling for
   `sensor_mode == 0x44` and the `< 20.0` threshold together make **millimetres** the consistent
   reading. **[I]**
2. Meaning of `sensor_mode` `0x44` vs `0x82`. Default is `0x82`. Likely sensor revisions or
   temperature sub-ranges — the two branches differ only in the distance-weighting constants. **[?]**
3. `fix_mode == 0x78` — presumably "fixed-pattern noise correction enabled". **[?]**
4. **~~New: whether the device derives the `userArea` coefficients from its own flash calibration
   (`MILI6_*.bin` / `tau_*.bin`) or from an on-die calibration.~~** — **RESOLVED [V]** 2026-09-20:
   neither, as posed. `tau_*.bin` / `MILI6_*.bin` are **host-side Windows SDK files**; they are
   never sent to the device and the Android build does not ship them at all. The `userArea`
   coefficients arrive **in the frame's own reference band**, so the device supplies them from its
   own calibration. See §9.
5. **New:** the `SN[2] == 'C'` test in `do_preview` (sets `UVCPreviewIR+0x219`). The serial
   families `DYTEPK78` (SN[2] = `T`) and `DYCRPK78` (SN[2] = `C`) are distinguished, so this is a
   **hardware-variant flag** — but which variant is unknown. **[?]**

---

## §8 — DYT container field semantics **[?]**

**Priority: low.**

`06-asset-and-file-formats.md` §2.2 gives the exact byte offsets of every sub-structure in the DYT
header, but not the meaning of the individual fields inside them. Filling this in means reading
the consumer code in `libUVCCamera.so` / `jpegext.dll` and matching field usage against the
thermometry parameter list.

---

## §9 — Which atmospheric-transmittance model is correct? **[?]**

**Priority: low — does not block the port.** Found 2026-09-20. Full write-up in
`10-calibration-tables.md` §3.4, §4.5, §8.3, §8.4.

The two vendor builds compute atmospheric transmittance by **two structurally different methods**:

| | Android (`libthermometry.so`) | Windows (`libirtemp.dll`) |
|---|---|---|
| method | closed form, `CalcFixRaw` | table lookup only |
| ships a table? | **no** | yes (`tau_*.bin`, `MILI6_*.bin`) |
| input space | ambient, humidity, distance | **target temperature**, distance |

Evidence that the Android path is table-free: the APK contains no calibration table of any kind,
and `libthermometry.so` is 13,856 bytes total — less than twice the 7,168-byte table — so it cannot
contain one. Conversely `libirtemp.dll` contains none of the analytic model's coefficients, and its
only `exp` call site is in `calculate_vapor_pressure`, not transmittance. **[V]**

**Why it does not block the port.** `tools/thermometry_diff` is byte-verified against the ARM
library, so the port follows the analytic path and that is the only model with byte-exact ground
truth. `linux-port/src/calib.{h,c}` is complete and tested but **deliberately unwired**: it is a
second thermometry backend, not a drop-in for `CalcFixRaw`, because `libirtemp` feeds the Q14
integer into its own differently-structured inversion.

**Two further problems found in the data itself [V]:**

1. **`tau_*.bin` is degenerate.** 48 of its 56 rows are bit-identical from 3.00 m all the way to
   50 m; the other 8 hold out to 5.00 m. Zero rows vary to the last column. `MILI6_*.bin` has no
   such plateau — all 56 rows vary to the end. So `tau_*.bin` must not be trusted beyond ~3 m.
2. **`tau_*.bin` is periodic with period 14 rows**, i.e. 4 byte-identical humidity planes × 14
   environment temperatures, which is the decomposition `read_tau` uses — not the target-temperature
   axis its own reader assumes. `MILI6_*.bin` is not periodic.

`MILI6_*.bin` has neither problem and belongs to the same SDK generation as `libirtemp.dll`, which
consumes it. If a table model is adopted, `MILI6_*` is the one to use.

**Next step — needs the device.** A blackbody comparison, whose design has been dry-run-validated
(§8.4 of `10-calibration-tables.md`) so that no hardware time is wasted:

* source at **300 °C** — not negotiable; at 50 °C the models are only 1.2× the noise floor apart and
  at 100 °C only 2.2×, both too marginal to conclude, whereas 300 °C gives 41.7×;
* sample at **1 m, 2 m and 10 m**, averaging several frames per distance;
* record ambient and humidity (the analytic model needs them; `humi` is a 0…1 fraction);
* the discriminator is the apparent-temperature **drop** from 2 m to 10 m, which cancels emissivity
  and absolute-calibration error: ~1.6 K (analytic) / ~10 K (`tau_H`) / ~28 K (`MILI6_H`).

Harness is ready. The tool compares the 2nd and 3rd distances given, so:

```bash
make build/taucmp
./build/taucmp -d calib --sim 300 --dist 1,2,10     # primary: discriminates 2 m -> 10 m
./build/taucmp -d calib --sim 300 --dist 0.5,1,10   # stronger variant: 1 m -> 10 m (65x noise)
```

---

## Prioritised action list

**Status as of the fourth session (2026-09-20): §1 is closed, §3 is decoded, the thermometry port is
byte-verified, and the bring-up + capture layers are written — all statically, no hardware.** The
calibration tables were the last substantive unknown; decoding them also produced a negative result
that narrowed the port (§9). **There is no remaining high-value no-hardware work.** The project is
now blocked on the device.

### Can be done now — no device required

Only three items remain, none on the critical path.

| # | Action | Effort | Value |
|---|---|---|---|
| 1 | Decode `block_lut.dat` with a .NET decompiler (ILSpy/dnSpy) | hours | low — no native code reads it, so it does not affect the port |
| 2 | Read `AES::DecryptionAES` key-schedule setup in `libDYTJpegAes.so` | minutes | low — §5, a licence check we do not reproduce |
| 3 | Identify `libMNN*` / `M1.exe` / `M2.exe` / `blk205` | low priority | low — §6 |

> **All three are curiosity items.** The next real step is the device.

### Completed — static work, no hardware

Recorded here for provenance, not as a to-do list. Each entry names where the evidence lives.

| Session | Item | Evidence |
|---|---|---|
| 1–2 | Thermal frame buffer layout solved | §1; `04-usb-protocol.md` §4.5 |
| 1–2 | Thermometry port written and byte-verified at widths 240 / 256 / 384 / 640, both `fix_mode`s | `08-linux-port-plan.md` §8.6.1; `make check` runs 8 `dumpframe --check` cases |
| 3 | Bring-up tooling: `Makefile`, `tools/probe.c` (read-only, refuses WRITE opcodes, `--selftest`), `src/serial.c` | `make check` |
| 3 | `src/control.c`, `src/frame.c` and the libuvc capture layer (`third_party/libuvc` v0.0.8 vendored) | `control_test`, `frame_test` — **`capture.c` is built but never run by `make check`, since it needs the device** |
| 4 | `tau_*.bin` / `MILI6_*.bin` decoded and extracted to `linux-port/calib/` | `10-calibration-tables.md` |
| 4 | `src/calib.{h,c}` implemented + tested | `calib_test`, 14,904 checks |
| 4 | τ-model divergence quantified; hardware experiment designed and dry-run-validated | `tools/taucmp.c`; `10-calibration-tables.md` §8.3, §8.4 |

**Explicitly *not* completed, despite appearing in earlier revisions of this list:**

* **`block_lut.dat`** — never decoded. It stays `[?]` in `10-calibration-tables.md` §7. Only its
  *irrelevance* was established: no native thermometry code references it, and the only mention is a
  UTF-16 string in the .NET assembly `CA30D.exe`. Item 1 above is the same work.
* **`Temperature.dll`** — never needed. An earlier revision of this list bundled it with
  `libirtemp.dll` as a target for disassembly; the calibration work ended up using `libirtemp.dll`,
  `Tiny1CDll.dll` and `DcontrolDll.dll`, and `Temperature.dll` contributed nothing.
* **`capture.c` is not test-covered.** It compiles and links, but no test drives it, because doing so
  requires a device. Do not read the `make check` gate as validating the capture path.

### Requires the device

| # | Action | Effort | Unblocks |
|---|---|---|---|
| 1 | `lsusb -v` — identify VID/PID, interfaces, `bFormatIndex` per stream | minutes | §4, and Phase 0 of `08-linux-port-plan.md` |
| 2 | **If the unit is `1514:0001` (mode `0x44C`), complete the serial-number handshake** — without it the device emits no thermal frames | hours | first live frame at all |
| 3 | First live frame through the frame→temperature pipeline | hours | end-to-end validation, and the first real exercise of `capture.c` |
| 4 | `usbmon` capture of a live preview | hours | the residual §1 sub-questions (stream demux) |
| 5 | Read a real `getTinyCParams` 15-byte response | hours | §2, and part of §7 |
| 6 | **The 300 °C blackbody comparison** (§9) — resolves which τ model is correct | hours | §9, and whether `calib.c` should be wired in |

---

## Verification discipline

This document set was begun in a single reconnaissance session and has since been revised across
several more (the most recent on 2026-09-20). Corrections are recorded inline as
**Correction**/**Superseded** notes rather than silently rewritten, so that a reader can see what
was believed and why it changed. Completed work is listed once, in the "Completed" subsection of the
action list — *not* as struck-through rows in the open-work table, because a table titled "can be
done now" that is two-thirds already done is worse than useless. Before trusting any specific claim:

* **`[V]` claims** have a command or an evidence path attached — re-run it.
* **`[I]` claims** are hypotheses. They are consistent with the evidence but not proven.
* **`[?]` claims** are explicitly unknown.

This repository has previously contained **fabricated** reverse-engineering write-ups. The tagging
exists so that a future reader can tell the difference without redoing the whole analysis. If you
find a claim here that does not reproduce, **correct the document rather than working around it.**

Three concrete examples from this document set, all caught by re-checking rather than by trust:

* §3 originally described `blk162`/`blk163` as arrays of 1792 IEEE-754 floats. They are Q14
  `uint16`. The float reading came from reinterpreting the first two `uint16` as one `float`.
* §1 originally asserted there was no user area inside the pixel plane. There is — the last 4 rows.
* An earlier revision of the action list marked "disassemble the loaders for `tau_*.bin`,
  `MILI6_*.bin`, `block_lut.dat`" as **DONE**. Two of the three were done; `block_lut.dat` was not,
  and `Temperature.dll` — also named in that row — was never touched. Bundling several items into
  one completion claim is how a list starts lying about itself. The claim has been split and the
  unfinished part moved back into the open table.
