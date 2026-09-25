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

> **Scope note (2026-09-24).** Everything above describes the **mode `0x44c`** frame — the reference
> band, the calibration record and the LUT all belong to that path. This unit is mode `1000`, whose
> payload has no reference band at all: `out[k] = raw[k]/64 - 273.15` over the whole plane. Mode
> `1000` also has two *output modes* that differ in geometry, and the port's default is the
> order-free one — a `256 × 384` frame whose top half is the device's grayscale visible image and
> whose bottom `256 × 192` is the thermal plane. See `04-usb-protocol.md` §4.10 and
> `linux-port/testdata/README.md`.

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

> **Correction (2026-09-24, live device) — all three are answered; §1 is now fully closed.**
> See `04-usb-protocol.md` §4.5.6 for the detail. In short: the thermal stream is
> **`bFormatIndex 1`** (the only format — `UNCOMPRESSED` YUY2, 16 bpp), 256×192 on `bFrameIndex 1`
> at 25 fps; there is **no** visible/thermal demux at the UVC level (one VideoStreaming interface,
> one format — the "dual vision" is an app-side composite); and the `+0x20` tag stays a host-side
> struct field. Note `bFrameIndex 2` (256×384) is a **synthetic decoy** — one distinct row and 3
> values in a 4-pixel cycle — so always select frame 1.

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
| `14 85 00 <idx> …` | read parameter slot `idx` | high — `getTinyCParams`; 16 slots now measured, §4.2 |
| `14 c5 …` | write one runtime parameter slot | high — `sendTinyCParamsModification`; **live-verified to take effect**, §4.2 |
| `01 82 00 7f f0 00 00 0f` (order id `0x14`) | read raw **user** serial | high — `getTinyCUserSn`; live-verified |
| `05 84 07 00 00 10 00 10` (order id `0x15`) | read **module** serial | high — `getTinyCRobotSn`; live-verified |
| `14 83 …` / `14 c3 …` | unknown pair used by `do_tinyC_order` case 7 | **low** |
| `0d 8b …` / `0d c1 …` | unknown; `0d c1` also used by `getTinyCUserData` (returns **1 byte**) | **low** |
| `00 00 00 00 00 00 00 02` | not a command — a receive-buffer pre-fill | high |
| 9-byte `00 00 01 00 01 80 19 00 02` | second stream-start step, written to `0x1d08` | high (function name) |

> **Correction (2026-09-24, live device + Windows capture) — one row is wrong, one is sharper.**
>
> * **`0a 01 …` (`setTinyCOutputADValue`) is *the* bring-up order**, not just "output raw AD
>   values". It is the one command that switches the device from its flat `0x8000` placeholder to
>   real thermal output, and it must be sent **after** streaming starts. It is also the only order
>   the Windows app sends during bring-up (`MechaniscoutPcap/4.pcapng`). Sent *before* streaming it
>   latches the device at status `0x0e` until a replug.
> * **`0f c1 …` (`tinyStartStream`) is not part of the vendor's startup at all** — nor is
>   `tinyStartStream2` or `getTinyCParams`. Their *meaning* is unchanged, but nothing in the port's
>   capture path sends them any more; sending them is what poisoned earlier sessions.
> * The `0x1d08` result-read model is **confirmed**, not doubted: the capture shows `14 85 00 03`
>   returning 16 bytes = ASCII `202605575259` + 4 zeros — the module serial. See
>   `04-usb-protocol.md` §4.2.

**Next step:** the parameter block layout. The 16-slot read-back is now measured
(`04-usb-protocol.md` §4.2), but only slots 1–5 have identified meanings (reflected/ambient K,
emissivity, distance, machine setting); slots 6–15 are raw values with no known semantics. Matching
them against the thermometry parameter list in `05-thermometry-algorithm.md` §5.3 would pin the
rest.

> **Correction (2026-09-25, live device).**
>
> * **The two "parameterised orders" are named functions, not unknowns.** Order id `0x14` is
>   `getTinyCUserSn` (raw user serial, 15 bytes) and order id `0x15` is `getTinyCRobotSn` (module
>   serial, 16 bytes). Both are live-verified — see `04-usb-protocol.md` §4.2/§4.8.
> * **`getTinyCUserData` returns 1 byte**, not 15. The earlier "15-byte user data read" claim was
>   an artefact of the shared result buffer.
> * **`DecryptSNE` is fully resolved** (and the published transcription was wrong). It decrypts the
>   raw user serial with a key derived from the last four digits of the module serial, and on the
>   reference unit yields the printable serial `DYCSTI09GG01292`. See `04-usb-protocol.md` §4.9 —
>   this closes the "serial-number decryption" question entirely.
> * **The `14 85 00 03` read is the emissivity slot** (index 3), not a fixed "parameter block"
>   command — byte 3 of the payload is the slot index, `0..15`.

> **Correction (2026-09-25, live device) — the write path is real, and it needs spacing.**
>
> * **`sendTinyCParamsModification` (`14 c5 …`) *is* applied at runtime.** In mode 1000 the
>   parameters are not metadata: writing emissivity `0.1` moved the frame mean from **32.7 °C to
>   73.9 °C** (+41.1 K), and distance `0.05 m` moved it to **100.9 °C** (+68.1 K); restoring each
>   returned the mean to baseline. Reflected `100 °C` and ambient `60 °C` move it the other way but
>   only by ~1.6 K / ~2.9 K. Full table in `04-usb-protocol.md` §4.2.
> * **This is a *runtime* write, not calibration.** It is two OUT transfers to `0x9d00`/`0x1d08`
>   with **no status poll**; it cannot touch factory data and it does not survive a replug. The
>   persistent writers (`setTinySaveCameraParams`, `setMachineSetting`) remain **deliberately not
>   implemented** — see `04-usb-protocol.md` §4.2.
> * **Consecutive orders must be spaced ≥ 100 ms.** Two orders sent back-to-back (0 ms gap) lose the
>   **first** one — only the second is applied. A 100 ms gap applies both; the port uses **250 ms**
>   (`DYT_ORDER_SETTLE_US`) for margin. This is a fixed gap, not a status poll: whether `0x0200`
>   signals write completion is still unverified, and the vendor's own `sendOrder` never polls.
> * **`type N` writes slot `N`** (type 1 → slot 1, …), verified with single writes followed by a
>   `getTinyCParams` read-back of the same index.

---

## §2a — What the parameter *values* mean **[I]**

**Priority: low — no longer gates the port.** The read-back map (`04-usb-protocol.md` §4.2) names
slots 1–5; the write encoders are now live-verified. What remains is the *semantic* meaning of
slots 6–15 and the exact units the device expects for each order type.

| order type | slot | natural unit | on-wire encoder | live effect |
|---|---|---|---|---|
| 1 | 1 | reflected (apparent) temperature, °C | `(int)(v + 273.15)` | small (−1.6 K at 100 °C) |
| 2 | 2 | ambient temperature, °C | `(int)(v + 273.15)` | small (−2.9 K at 60 °C) |
| 3 | 3 | emissivity, `0…1` | `(int)(v * 128.0)` | dominant (+41.1 K at 0.1) |
| 4 | 4 | distance, metres | `(int)(v * 128.0)` | dominant (+68.1 K at 0.05 m) |

Truncation is toward zero (C cast), not floor. Non-finite and out-of-range values are rejected by
the port before they reach the wire. The exact *physical* calibration (why distance `0.05` raises the
apparent temperature) belongs to the thermometry model, `05-thermometry-algorithm.md`.

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

## §4 — Which VID/PID is this unit? — **RESOLVED [V] (2026-09-24)**

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

> **Correction (2026-09-24, live device) — the inference above is wrong; §4 is closed.**
> `lsusb` answers it: the unit enumerates as **`0bda:5840`** → mode **`1000`**, *not*
> `1514:0001`/`0x44C`. So the serial handshake above **does not apply** — `isVerifySN` is gated on
> the radiometric modes only. Full sequence in `04-usb-protocol.md` §4.10; the same `1514:0001`
> reasoning was corrected in `01-target-and-hardware.md` §1.3.
>
> **Correction (2026-09-24, later the same day) — the `setTinyCOutputADValue` claim above is also
> wrong.** It is not a bring-up step the device needs; it is a **mode switch**. In the device's
> default mode the 256x384 frame already carries real data — top half a grayscale visible image,
> bottom half the thermal plane — and it needs no vendor order at all. Sending the order *replaces*
> that with a flat 256x192 raw-AD frame and turns the 256x384 frame into a placeholder. Both modes
> carry the same thermal data (A-B-A interleave: cross-mode correlation 0.94 vs a 0.93 same-mode
> noise floor), so the port now defaults to the order-free dual-half mode and keeps AD behind
> `--ad-output`. This is why Thermal-Camera-Redux works unmodified on this unit. See
> `04-usb-protocol.md` §4.10 and `linux-port/testdata/README.md`.

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

> **Cross-check (2026-09-25).** The `DecryptSNE` decode of the reference unit's raw user serial is
> `DYCSTI09GG01292` (`04-usb-protocol.md` §4.9) — the same `DYCSTI09` prefix as the
> `dyt1101c` / `dyt0526cdyt0526c` row above. Two independent mechanisms (the AES serial path and
> `DecryptSNE`) agree on the serial prefix, which is a strong confirmation that the corrected
> `DecryptSNE` transcription and its `mod = 52` key are right.

---

## §6 — Unidentified artifacts **[?]**

**Priority: low.**

| Artifact | Question |
|---|---|
| `libMNN.so`, `libMNN_Express.so`, `libmnnmodel.so` | an on-device neural-network runtime. **Resolved 2026-09-25 — see the note below: a 2× super-resolution model whose weights are embedded (XOR-obfuscated) in `libmnnmodel.so`; recovered to `linux-port/models/zoom2.mnn`.** Irrelevant to basic capture. |
| `M1.exe`, `M2.exe` | shipped in the Windows installer; purpose unknown |
| `blk205` (23,618 bytes, non-PE) | unidentified; sits next to the NSIS uninstaller (`blk206`) |
| `blk165`, `blk168` (PDB) | `[I]` almost certainly `ThermalAnalysis.pdb` / `ThermalAnalysisSystem.pdb` — the only two PDB blocks and the only two `.pdb` names in the file list — but not byte-confirmed |
| `libPhotoNativeHelper.so`, `libsimplePictureProcessing.so` | Android-only helpers; roles not examined |
| `6.dat` palette | ships on Android but has no Windows equivalent |
| `7.dat` palette | ships on Android but is absent from `DYConstants.paletteArrays` |

> **The MNN row, answered (2026-09-25).** It is a 2× super-resolution model, and its weights are
> **embedded, obfuscated, inside `libmnnmodel.so`** — not shipped as a file.
>
> The purpose is clear from the Java surface: `com.energy.mnnmodellibrary.MImageUtils` exposes only
> `MNN_ZOOM_X2`, a **2× zoom**, called from `AbstractUVCCameraHandler` via
> `MRun2(MNN_ZOOM_X2, in, out)` with `out = new byte[393216]`; `MRun1(MNN_ZOOM_X2, 2)` sets it up and
> `MRun3` tears it down. So the engine's Phase 8 ("optional 256×384 super-resolution") had the right
> reading of it.
>
> **The weights were found by tracing those call sites into the native library.** `native_mnn_run_1`
> dispatches to `mnn_run_1(1, level)`, which calls `sr1(level, key)` — and `sr1` (at `0x6010`, 820
> bytes) does the whole job:
>
> ```
> sr1:  buf = operator new[](size)                     @ 0x6034-0x6044
>       for i in 0..size:                              @ 0x605c-0x60b8
>           buf[i] = .data[i] ^ key[i % 25]            # 25-byte repeating XOR
>       MNN::Interpreter::createFromBuffer(buf, size)  @ 0x60cc
> ```
>
> with the ciphertext being the whole of `.data` from VA `0xf120`, its length a `uint32` global at VA
> `0x123a0` (12,928), and the key 25 bytes at `.rodata` VA `0x22e0`. Note the asymmetry that makes
> this easy to miss: `MNN::Interpreter::**createFromBuffer**` is imported, `createFromFile` is not,
> and `native_init_mnn_model_module(Context)` is a **40-byte no-op** that sets a flag and ignores its
> `Context` — so nothing about the API surface says "embedded".
>
> The decrypted blob is a 12,928-byte MNN flatbuffer, an ESPCN-style network exported from ONNX —
> `image_input` → `onnx::Conv_14/16/18/20/22` → `Add_23/25` → `DepthToSpace_26` → `Clip_27` →
> `image_output`, asset UUID `7006ec85-d318-4f47-9508-fbbe5f08ec82`. `DepthToSpace` with block size 2
> is the pixel-shuffle upsampler, which is what makes it a zoom rather than a resample.
>
> **The shape contract is pinned from three agreeing sources**, one of which is the model's own
> reshape: `sr1` builds a dims vector and calls `resizeTensor` with **`[1, 1, 192, 256]`** (NCHW,
> one channel, 256×192 — the input geometry is set at load time, not baked into the flatbuffer); the
> app allocates `393216` = 512 × 384 × 2 bytes; and `DepthToSpace(2)` on a single-channel output
> turns 256×192 into exactly 512×384.
>
> **It is now in the tree**, recovered reproducibly rather than copied:
>
> * `RE Workspace/tools/extract_mnn_model.py` — re-derives it from `libmnnmodel.so`, asserting each
>   address falls inside the section it belongs to so a changed library fails loudly.
> * `linux-port/models/zoom2.mnn` (12,928 bytes, sha256 `6a86cf01…`) with `models/README.md`
>   documenting the provenance, the graph and the contract.
> * `linux-port/src/mnn.{h,c}` — the contract as data, a runtime-free structural validator, and an
>   upscale call that refuses rather than inventing a frame. `src/mnn_test.c` (23 checks) runs in
>   `make check`.
>
> **Execution, closed 2026-09-25.** The model now runs on this host. `src/mnn_runtime.cpp` holds the
> MNN C++ integration behind the `dyt_mnn_zoom2()` seam; the Makefile compiles it only when an
> upstream MNN install is present (`HAVE_MNN`, recipe in `linux-port/third_party/README.md`), and
> without one the seam refuses as before. The six calls are the ones read out of `sr1`:
> `createFromBuffer` → `createSession` → `getSessionInput` + `resizeTensor([1,1,192,256])` →
> `getSessionOutput` → `runSession`, plus `resizeSession`.
>
> The numeric behaviour is now **verified against the vendor's own `mnn_run_2`**, driven on this host
> out of `libmnnmodel.so` by `tools/mnn_diff/`. On a fixed non-constant plane the port reproduces the
> vendor on **177081 of 196608 samples bit-exactly, with a maximum error of one LSB**; the residual is
> deterministic drift between the vendor's MNN 2.5.0 and upstream MNN, unchanged by thread count or
> precision mode. `make check` runs this comparison, and it has teeth: a layout regression moves the
> peak error to 253.
>
> Two findings from doing it, both in `models/README.md`:
>
> * The uint16↔float convention is pinned from the vendor's disassembly and confirmed empirically —
>   input is the **low byte** of each uint16 divided by 255.0; output is `(uint16_t)(f + 32768.0f)`,
>   truncated. The network has unit DC gain, so the bias is a pure offset.
> * **The session tensor is NC4HW4 while reporting `getDimensionType() == CAFFE`.** For the 1-channel
>   192×256 input `elementSize()` is 196608, not 49152 — the channel dim is padded to 4 — so writing
>   samples straight into `host<float>()` scrambles the frame. A *constant* test plane cannot detect
>   this, which is why the differential uses a non-constant one.
>
> Still open: **which plane is upscaled** (the Java feeder is `onYUVtoJava`, suggesting the *visible*
> half, not the thermal one — the differential pins the arithmetic, not the input's identity).
>
> **A correction worth recording.** An earlier revision of this note concluded the weights were
> "not shipped in this APK" and that Phase 8 was blocked on finding them. That was **wrong**, and
> the reasoning was the mistake: the absence of a `.mnn` file in `assets/`, `res/raw` and the
> installer was established correctly, but "the runtime is only 68 KB, so there is no room for a
> model" was an inference from size rather than a check, and it was false — 12,928 bytes fit
> comfortably. The lesson is the one this document exists for: absence of an artifact in the
> obvious places is not absence of the artifact. What actually resolved it was reading the call
> sites instead of the file listing.

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

**The container itself is no longer in question.** `06-asset-and-file-formats.md` §2.1 is now
verified by running the vendor's own `D_updateData` on this host: the container it writes is
byte-identical to the one `src/dytjpeg.c` writes from the same inputs, and the port's reader
recovers the vendor's blob, payload and original image exactly (§2.5 there,
`tools/dytjpeg_diff/run.sh`). What remains `[?]` is only the *meaning* of the metadata fields the
port does not use.

Two side-findings from that work are recorded in §2.3 and §2.5 of that document:

* Both vendor **readers** (`D_getDytFileLength`, `D_jpegOpen`) call `free()` on their segment
  cursor rather than the buffer base. glibc aborts or segfaults on that; bionic tolerates it. They
  are dead code in the app, which is why it was never noticed. The differential therefore skips the
  reader direction rather than patching the code under test.
* Making the library loadable at all required three loader-compatibility fixes (NULL
  `.init_array` entries, `p_align = 0x1000`, and two `PT_LOAD`s sharing a page on 16/64 KiB hosts),
  all in `tools/vendor_shim/prep_vendor_so.py`. None of them touches what the library computes.

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

**Status as of the eighth session (2026-09-25): the port's engine is complete — all eight phases of
the engine roadmap have landed — and it runs against real hardware.** Thermometry is byte-verified,
the capture path is live, the device's serial and stored parameters have been read, the one runtime
write (`sendOrder`) is implemented and measured, the dual-half visible plane plus all six fusion
patterns are implemented and verified live, the DYT still container is written and read by the port
with its bytes pinned against the vendor's own writer, video recording writes H.264 mp4 from both the
device and a frozen fixture, and the vendor's 2× super-resolution model has been recovered from
`libmnnmodel.so` with its shape contract pinned. The one thing that cannot be finished here is
*executing* that model — there is no MNN runtime on this host (§6) — and the one thing that still
needs the bench is §9's blackbody comparison, plus anything only a second hardware unit can answer.

> The engine work is tracked as its own roadmap (display foundation → measurement/alarms → device
> read-back → the one device write → visible-half fusion → DYT container → mp4 recording → MNN),
> **separate from the phase numbering in `08-linux-port-plan.md`**, which covers the original
> bring-up and thermometry phases 0–5. Do not conflate the two schemes.

### Can be done now — no device required

Three RE curiosity items remain, none on the critical path, and **all eight engine phases have
landed** — the engine roadmap is closed. The only engine work left anywhere is *executing* the
recovered super-resolution model, which needs an MNN runtime this host does not have (§6).

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
| 5 (2026-09-25) | **Live device reads verified**: module serial, raw user serial, and the full 16-slot parameter map; `getTinyCUserData` corrected to 1 byte | `04-usb-protocol.md` §4.2/§4.8; `linux-port/tools/probe.c --info` |
| 5 (2026-09-25) | **`DecryptSNE` corrected and the user-serial decode verified** (`DYCSTI09GG01292`) | `04-usb-protocol.md` §4.9 |
| 5 (2026-09-25) | `src/params.{h,c}` (parameter codec + radiometry snapshot) and the read helpers in `src/control.{h,c}` | `params_test`, `control_test` |
| 5 (2026-09-25) | **Runtime parameter writes implemented and live-verified**: `dyt_params_build_cmd` (pure), `dyt_write_param` (2 OUT, no poll), `dyt_capture_set_param` (settle gap); device confirmed to apply emissivity / distance / reflected / ambient | `04-usb-protocol.md` §4.2; `params_test`, `control_test` |
| 5 (2026-09-25) | **`sendTinyCParamsModification` measured**: does move the reading in mode 1000 (emissivity 0.1 → +41.1 K; distance 0.05 m → +68.1 K); consecutive orders need ≥ 100 ms spacing | `04-usb-protocol.md` §4.2; this §2 correction |
| 5 (2026-09-25) | **Phase-4 UI exposure verified live**: `e`/`A`/`R`/`D` arm a value and `y` sends it; all four types reach the device (read-back confirmed), and `n`/ESC cancels without writing | `tools/dytview.cpp`; `04-usb-protocol.md` §4.2 |
| 5 (2026-09-25) | **Parameters are volatile**: a device reset reverted reflected/ambient from 300 K to 273 K with no port write; the capture path does *not* reset them | `04-usb-protocol.md` §4.2 |
| 6 (2026-09-25) | **Visible half + fusion implemented** (engine Phase 5): `src/visible.{h,c}` (YUYV luma extraction), `src/fusion.{h,c}` (all six patterns + X/Y alignment), the read-only `dyt_capture_plane_geometry()`, the `src/session_capture.{h,c}` adapter, and the session's visible-plane/fusion state | `visible_test`, `fusion_test` (+ golden PPM), `session_test`, `pipeline_test` |
| 6 (2026-09-25) | **The two halves of the dual-half payload are asserted, not assumed**: `dyt_visible_is_grey()` says yes for the top half and no for the thermal bottom half — one predicate, opposite answers | `pipeline_test` (`test_dual_half_halves`), `visible_test`; `04-usb-protocol.md` §4.10 |
| 6 (2026-09-25) | **Fusion verified live in all six patterns** and the alignment measured: `dx=+2` moved the visible plane exactly 2 source px left (cross-correlated at ×2 zoom), matching the vendor's `X_Coefficient` sense | `04-usb-protocol.md` §4.10; `03-android-app-architecture.md` §3.5.2 |
| 6 (2026-09-25) | **AD-mode fallback verified live**: with no visible half the pattern is reported as `(no visible plane)` and the render falls back to thermal rather than fusing nothing | `04-usb-protocol.md` §4.10 |
| 7 (2026-09-25) | **Engine Phase 6 — the DYT still container implemented**: `src/jpeg.{h,c}` (stb default, libjpeg optional), `src/dytjpeg.{h,c}` (container write/read, codec-free), the optional PNG writer, the session's own raw-payload copy, and `dytview`'s `s` (DYT still) / `w` (window PNG) | `jpeg_test`, `dytjpeg_test` (132 assertions), `imgwrite_test`, `session_test`, `pipeline_test` |
| 7 (2026-09-25) | **The container is verified against the vendor's own writer**: the container `D_updateData` produces is byte-identical to `dyt_dyt_build`'s for the same inputs, and the port's reader recovers the vendor's blob/raw/JPEG exactly — with a synthetic multi-chunk payload and the frozen 196608-byte device frame | `06-asset-and-file-formats.md` §2.5; `tools/dytjpeg_diff/run.sh` |
| 7 (2026-09-25) | **A live still written by `dytview`'s `s` key verified end-to-end**: 5 APP2 segments (a 1656-byte blob, three `0xffff` full chunks, one 9-byte partial), geometry record 256×192/384 dual-half, and rebuilding the container from the recovered parts reproduces the file byte-for-byte | live run against `0bda:5840`; `06-asset-and-file-formats.md` §2.5 |
| 7 (2026-09-25) | **Two latent bugs found in the vendor's DYT readers**: both `D_getDytFileLength` and `D_jpegOpen` free their segment cursor instead of the buffer base, which glibc does not tolerate. Dead code in the app (the only JNI wrapper names an absent package), so never observed | `06-asset-and-file-formats.md` §2.3 |
| 7 (2026-09-25) | **The vendor DYT library made loadable on 4/16/64 KiB hosts** and the bionic shim rewritten as one shared copy (`tools/vendor_shim/`), with `LIBC`/`LIBC_N` nodes, the C23 `strtol` redirect disabled, and a corrected `abort`; the rewritten shim reproduces the frozen thermometry ground truth byte-for-byte (old shim == new shim, 4 artifacts × 4 widths) | `06-asset-and-file-formats.md` §2.5; `tools/vendor_shim/` |
| 8 (2026-09-25) | **Engine Phase 7 — mp4 recording implemented**: `tools/dytrec.cpp` records `dyt_session_render_rgb()` frames via `cv::VideoWriter`, and `tools/frame_source.{h,c}` abstracts the source (live device vs a frozen `.raw` replayed through the device-free pipeline) so the recorder is testable with no hardware | `dytrec --selftest`; `tools/frame_source.h` |
| 8 (2026-09-25) | **The recorder needs no new dependency**: `opencv_videoio` was already linked for the viewer, and its bundled ffmpeg writes real H.264 (`codec_name=h264`) on this host — so no libavformat/libx264 dev package is required. `--codec mp4v` is offered as the fallback | `dytrec --help`; verified with `ffprobe` |
| 8 (2026-09-25) | **`dytrec --selftest` is the offline check, and it runs in `make check`** (under `HAVE_OPENCV`): one pass over `testdata/mode1000_256x384_default.raw` that encodes N frames and asserts the frame count, the 256×192 render geometry, and that the temperatures are the fixture's real 31.41..32.41 °C rather than the device's ~238.85 °C start-up filler | `dytrec --selftest` (4/4 ok); `Makefile` `check` |
| 8 (2026-09-25) | **Live recording verified against `0bda:5840`**: default dual-half mode, filler cleared at frame 154, 5 s settle, then 75 frames — `ffprobe` reports h264 256×192 25/1 `nb_frames=75` and `duration=3.000000`, and decoded frames 0 and 74 differ, so the clip is live data and not a held frame | live run; `tools/dytrec.cpp` |
| 8 (2026-09-25) | **Engine Phase 8 — the super-resolution model recovered**: the weights are embedded, XOR-obfuscated, in `libmnnmodel.so`; traced from the Java call sites into `sr1`, which decrypts and calls `MNN::Interpreter::createFromBuffer`. Extracted reproducibly to `linux-port/models/zoom2.mnn` (12,928 bytes) — an ESPCN-style ONNX export with `DepthToSpace(2)` | `RE Workspace/tools/extract_mnn_model.py`; `linux-port/models/README.md`; §6 |
| 8 (2026-09-25) | **The super-resolution shape contract pinned from three sources**: `sr1`'s `resizeTensor([1,1,192,256])`, the app's `new byte[393216]` output buffer, and `DepthToSpace` block size 2 — 256×192 → 512×384 | `linux-port/models/README.md` |
| 8 (2026-09-25) | **The Phase 8 seam landed and is optional**: `src/mnn.{h,c}` (contract as data, runtime-free flatbuffer validator, an upscale that refuses rather than inventing a frame) + `src/mnn_test.c` (23 checks) in `make check`. Running the model still needs an MNN runtime, which this host does not have — `mnn.c` records the six-call integration from `sr1` | `src/mnn_test.c`; `models/zoom2.mnn` |

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
| 5 | ~~Read a real `getTinyCParams` response~~ — **done 2026-09-25**: all 16 slots read, §4.2 | — | §2 (slots 1–5 identified; 6–15 still raw) |
| 6 | **The 300 °C blackbody comparison** (§9) — resolves which τ model is correct | hours | §9, and whether `calib.c` should be wired in |
| 7 | ~~Whether `sendTinyCParamsModification` actually changes the reading~~ — **done 2026-09-25**: it does (emissivity/distance dominate); spacing constraint measured | — | §2, §2a |
| 8 | **Measure the true visible/thermal registration offset.** The port assumes the dual-half top half is already 1:1 with the thermal plane (alignment default `0,0`) — that is an *assumption*, not a measurement. Needs a scene with contrast in *both* planes (a hand in front of a warm background): find the offset that best aligns the visible edge to the thermal edge, then set the default. | hours | engine Phase 5's alignment default; §4.10 |

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
