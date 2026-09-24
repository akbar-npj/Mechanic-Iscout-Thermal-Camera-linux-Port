# 04 — USB / UVC Protocol

> **This is the primary porting reference.** Everything here is **[V]** (reproduced from
> `libuvc.so` and `libUVCCamera.so` decompilation) unless tagged otherwise.

## 4.1 Transport model

The device is a **USB Video Class (UVC) camera** driven by a **modified upstream libuvc**.

**[V]** `libuvc.so` embeds its own source paths:

```
jni/libuvc/android/jni/../../src/init.c
jni/libuvc/android/jni/../../src/device.c
jni/libuvc/android/jni/../../src/ctrl.c
jni/libuvc/android/jni/../../src/stream.c
jni/libuvc/android/jni/../../src/frame.c
jni/libuvc/android/jni/../../src/diag.c
```

So the codebase is upstream libuvc (the well-known `libuvc` by Ken Tossell / saki4510t's
Android port) **plus vendor additions**. On Linux, upstream libuvc builds against
`libusb-1.0` natively — this is why the port is short.

### Vendor additions to libuvc **[V]**

Exported symbols present in `libuvc.so` that are **not** in upstream libuvc:

| Symbol | Notes |
|--------|-------|
| `uvc_diy_communicate` | **the vendor control-transfer primitive** (§4.2) |
| `uvc_diy_start_preview` | vendor preview start |
| `uvc_diy_stop_preview` | vendor preview stop |
| `uvc_find_device2` | extended device find |
| `uvc_allocate_ini_frame` | pre-allocate a frame buffer |
| `uvc_allocate_ini_preview_frame` | pre-allocate the preview buffer |
| `uvc_init2` | extended init |
| `uvc_already_open` | re-open guard |
| `uvc_get_stream_ctrl_format_size_fps` | convenience variant of upstream `uvc_get_stream_ctrl_format_size`; **not** in stock libuvc |
| `uvc_any2bgr`, `uvc_any2iyuv420SP`, `uvc_any2yuv420SP`, `uvc_any2yuyv`, `uvc_mjpeg2*` | colour conversion helpers |
| `GetMedianNum`, `MedianFilter` | 1-D median filter (hot-pixel / noise removal) |
| `uvc_get_device_with_fd`, `uvc_get_bus_number`, `uvc_get_device_address` | fd-based attach (Android `UsbDeviceConnection`) |

### UVC class control (standard)

Standard UVC camera controls are all present and unmodified — `uvc_get_brightness`,
`uvc_get_contrast`, `uvc_get_gain`, `uvc_get_exposure_abs`, `uvc_get_white_balance_*`,
`uvc_get_focus_*`, `uvc_parse_vc_extension_unit`, `uvc_parse_vs_format_uncompressed`,
`uvc_get_stream_ctrl_format_size`, … **[V]** These work on Linux with stock libuvc.

> **Correction (Linux port).** An earlier revision of this list included
> `uvc_get_stream_ctrl_format_size_fps`. That symbol is **not** in upstream libuvc — it is a vendor
> addition (see the table above). Upstream exports only
> `uvc_get_stream_ctrl_format_size(devh, ctrl, format, width, height, fps)`, which is the one a
> Linux port linking stock libuvc must call. **Verified:** `nm -D libuvc.so` lists *both* symbols
> (`0x14894` `_fps`, `0x14b58` unsuffixed), while upstream `v0.0.8
> include/libuvc/libuvc.h:599` declares only the unsuffixed form.

---

## 4.2 The vendor control primitive

**[V]** `libuvc.so`:

```c
int uvc_diy_communicate(uvc_device_handle_t *devh)
{
  int r = libusb_control_transfer(devh->usb_devh);   /* args passed through */
  if (r == 8) r = 0;                                 /* normalise 8-byte success */
  return r;
}
```

Ghidra lost the arguments because the function is a register-pass-through wrapper. The
**real signature** is recovered from the 41 call sites in `libUVCCamera.so`:

```c
int uvc_diy_communicate(uvc_device_handle_t *devh,
                        uint8_t  bmRequestType,
                        uint8_t  bRequest,
                        uint16_t wValue,
                        uint16_t wIndex,
                        uint8_t *data,
                        uint16_t wLength,
                        unsigned timeout);      /* always 1000 ms in observed code */
```

> **Quirk:** the wrapper only maps the return value `8 → 0`. For 1-, 2-, 15- and 28-byte
> transfers it returns the raw libusb byte count. Callers must compare against the expected
> length themselves. Reproduce this exactly or the timing changes.

### Request encoding (all observed values) **[V]**

| Field | OUT (host → device) | IN (device → host) |
|-------|--------------------|--------------------|
| `bmRequestType` | `0x41` (vendor, device, OUT) | `0xC1` (vendor, device, IN) |
| `bRequest` | `0x45` | `0x44` |
| `wValue` | `0x0078` (120) — **always** | `0x0078` — **always** |

### Register map (`wIndex`) **[V]**

| `wIndex` | Direction | Length | Role |
|----------|-----------|--------|------|
| `0x1d00` | OUT | 8 | **command register** — write the 8-byte order |
| `0x9d00` | OUT | 8 | alternate command register (used by stream start / param modification) |
| `0x1d08` | IN | 15 (`0xf`) | **result read** after a command |
| `0x1d08` | IN | 28 (`0x1c`) | extended result (`getTinyCUserSnCoefficient`) |
| `0x1d08` | IN | 2 | short result |
| `0x1d08` | OUT | 8 or 9 | parameter / stream-start write |
| `0x1d10` | IN | 2 | secondary read (machine settings) |
| `0x0200` | IN | 1 | **status byte** |

### Status byte semantics (`wIndex 0x0200`) **[V]**

From `UVCPreviewIR::doTinyCOrder` and `sendTinyCAllOrder`:

```c
for (i = 0; i < 1000; i++) {
    uvc_diy_communicate(devh, 0xC1, 0x44, 0x78, 0x0200, &status, 1, 1000);
    if ((status & 0x01) == 0) {          /* bit0 clear -> not busy */
        if ((status & 0x02) == 0) break; /* bit1 clear -> complete, done */
        if ((status & 0xfc) != 0)        /* bits2..7 set -> ERROR */
            return -1;
    }
}
```

| Bit | Meaning |
|-----|---------|
| 0 | **BUSY** — command still executing |
| 1 | **PENDING/READY** — result available; loop exits when clear |
| 2–7 | **ERROR** — any set bit aborts |

> **Correction (2026-09-24, live device).** The `[V]` above is *static* verification — the bit
> layout is a faithful decompilation of `doTinyCOrder` — and it does **not** reproduce against
> hardware. Two independent observations:
>
> * The Windows vendor capture (`MechaniscoutPcap/3.pcapng`, 356 s of normal operation, 689 status
>   polls) shows **only** `0x01` (busy) and `0x00` (done). Bits 2..7 are never set. The `0xfc`
>   abort branch is therefore a decompiled branch of **unconfirmed meaning**, not a hardware-verified
>   error path.
> * This unit (`0bda:5840`) answers `setTinyCOutputADValue` with `0x01`, then latches **`0x0e`** —
>   which the loop above would treat as ERROR and abort on. `0x0e` is a device state, not a
>   transient error (see §4.10's correction).
>
> Treat **`0x01` = busy / `0x00` = done** as the only verified semantics. The port's live bring-up
> deliberately does not abort on the documented error bits: `capture.c:order_trace()` logs the raw
> status progression instead.

### Canonical command transaction **[V]**

```
1. OUT  wIndex=0x1d00, 8 bytes   -> the command
2. poll wIndex=0x0200, 1 byte    -> until (b & 1)==0 && (b & 2)==0, max 1000 tries
3. IN   wIndex=0x1d08, 15 bytes  -> the result
```

> **Correction (2026-09-24) — the `0x1d08` "echo register" model was a wrong turn.**
> An intermediate session concluded from a latched device that `0x1d08` "echoes the last write" and
> that this transaction was therefore wrong or incomplete. **Both claims are false.** `0x1d08` *is*
> a command-result register: the Windows capture shows `14 85 00 03` (getTinyCParams) returning
> **16 bytes = ASCII `202605575259` + 4 zero bytes** — the module serial — and another order
> returning 15 bytes of encrypted SN. The "echo" was an artifact of the port writing 9 bytes
> (`tinyStartStream2`'s payload) to `0x1d08` while the device sat in the latched `0x0e` state
> (§4.10), after which every read returned those stale bytes. The transaction shape above is
> byte-identical to the vendor's (`linux-port/src/control.c:dyt_transaction_ex`) and needs no
> change. It was also what made three different commands appear to return identical data.

Observed command payloads **[V]** (from `UVCPreviewIR::doTinyCOrder`, the `switch` on the
JNI order id):

```
mode 0x14:  01 82 00 7f f0 00 00 0f
mode 0x15:  05 84 07 00 00 10 00 10
```

Byte 0 is an order class, byte 1 a sub-opcode, bytes 2..7 parameters.

### The opcode table — recovered **[V]**

The 8-byte command payloads are **static constants in `.rodata`**, copied into a stack buffer with
`memcpy` before each transfer. They can therefore be read straight out of the binary.

Address mapping for `libUVCCamera.so`: **`file_offset = VA - 0x100000`** (verified against a known
`.rodata` string). Dump them with:

```bash
cd "../RE Workspace"
python3 - <<'EOF'
b = open("apk/lib/arm64-v8a/libUVCCamera.so","rb").read()
for name, va, ln in [
    ("setMachineSetting  write", 0x23e1da, 8),
    ("setMachineSetting  read ", 0x23e1e2, 8),
    ("do_tinyC_order case 4   ", 0x23e1ea, 8),
    ("tinyStartStream         ", 0x23e1f2, 8),
    ("(unreferenced)          ", 0x23e1fa, 8),
    ("do_tinyC_order case 5   ", 0x23e202, 8),
    ("case 6 / UserData / 8000", 0x23e20a, 8),
    ("do_tinyC_order case 7 a ", 0x23e212, 8),
    ("do_tinyC_order case 7 b ", 0x23e21a, 8),
    ("OutputADValue / 8004    ", 0x23e222, 8),
    ("ParamsModification      ", 0x23e22a, 8),
    ("getTinyCParams / Save   ", 0x23e232, 8),
]:
    o = va - 0x100000
    print("%s VA %#08x : %s" % (name, va, " ".join("%02x"%c for c in b[o:o+ln])))
print("tinyStartStream2 (9 B)   VA 0x23cdbc :",
      " ".join("%02x"%c for c in b[0x23cdbc-0x100000:0x23cdbc-0x100000+9]))
EOF
```

Resulting table:

| Command bytes | `b0` | `b1` | Written to | Function | Purpose |
|---|---|---|---|---|---|
| `14 c5 00 05 00 00 00 00` | `14` | `c5` | `0x9d00` | `setMachineSetting` | write machine setting |
| `14 85 00 05 00 00 00 00` | `14` | `85` | `0x9d00` | `setMachineSetting` | read machine setting |
| `14 85 00 03 00 00 00 00` | `14` | `85` | `0x9d00` | `getTinyCParams`, `setTinySaveCameraParams` | read/save parameter block |
| `14 c5 00 00 00 00 00 00` | `14` | `c5` | `0x9d00` | `sendTinyCParamsModification` | write parameter block |
| `14 83 00 02 00 00 00 00` | `14` | `83` | `0x9d00` | `do_tinyC_order` case 7 | *(see case 7 below)* |
| `14 c3 00 02 00 00 00 00` | `14` | `c3` | `0x9d00` | `do_tinyC_order` case 7 | *(see case 7 below)* |
| `0f c1 00 00 00 00 00 09` | `0f` | `c1` | `0x9d00` | `tinyStartStream` | **start thermal stream** |
| `0d 8b 00 00 00 00 00 02` | `0d` | `8b` | `0x1d00` | `do_tinyC_order` case 5 | *(see case 5)* |
| `0d c1 00 00 00 00 00 00` | `0d` | `c1` | `0x1d00` | case 6, `getTinyCUserData`, `sendTinyCOrder(0x8000)` | user data read |
| `0a 01 00 00 00 00 00 00` | `0a` | `01` | `0x1d00` | `setTinyCOutputADValue`, `sendTinyCOrder(0x8004)` | **output raw AD values** |
| `00 00 00 00 00 00 00 02` | — | — | *(recv buffer)* | case 4, `setMachineSetting` | receive-buffer pre-fill, not a command |
| *(9 bytes)* `00 00 01 00 01 80 19 00 02` | — | — | `0x1d08` | `tinyStartStream2` | **second stream-start step** |

### The two parameterised orders **[V]**

`UVCPreviewIR::doTinyCOrder` (`@ 0x166e54`) builds its command **at runtime** from
`this+0xc48` (order id) and `this+0xc40` (a pointer to a 15-byte data buffer). This is the one
place the opcode is *not* a static constant:

**Order id `0x14`** — command `01 82 00 7f f0 00 00 0f`:
```
cmd[0] = 0x01
cmd[1] = 0x82
cmd[2..5] = big-endian u32  = 0x007ff000     (this+0xc40-independent constant)
cmd[6..7] = big-endian u16  = 0x000f
```
then poll `0x0200`, then **IN 15 bytes** into `*(this+0xc40)`.

**Order id `0x15`** — command `05 84 07 00 00 10 00 10` (hard-coded):
```
cmd = 05 84 07 00 00 10 00 10
```
then poll `0x0200`, then **IN 15 bytes** into `*(this+0xc40)`.

Both are read-style: 8-byte command out, 15-byte result in. The polling loop is the canonical
status loop from §4.2, with `max 1000` iterations. **[V]**

### `setMachineSetting` — a calibration coefficient write **[V]**

`UVCPreviewIR::setMachineSetting(int, int)` (`@ 0x167e3c`) calls
`sendTinyCParamsModification(this, &float_value, ..., 4)` with a **single float**:

| condition | float bits | value |
|---|---|---|
| `this[0xb2e] == 'C'` (robot serial's 3rd char) | `0x3f800000` | `1.0` |
| `param_2 == 0` | `0x3f5930be` | ≈ `0.848` |
| otherwise | `0x3f688ce7` | ≈ `0.908` |

It then loops up to `0x32` (50) times reading back via `0x9d00` / `0x1d08` / `0x1d10` and
comparing against the requested value, returning `1` on match. This is a **verified write path
into device calibration state** — treat it as dangerous (§4.8).

### Java order-id dispatch **[V]**

`UVCPreviewIR::do_tinyC_order(JNIEnv*)` (`@ 0x1691ec`) is a worker thread that waits on a condvar
and switches on `*(u32*)(this+0xb3c)`:

| case | action |
|---|---|
| `0` | `getTinyCUserSnCoefficient()` + `getTinyCUserSn()` |
| `1` | `getTinyCRobotSn()` then `DecryptSNE(...)` |
| `2` | no-op |
| `3` | `setMachineSetting(1, 1)` |
| `4` | OUT `0x1d00` (const `00 00 00 00 00 00 00 02`), IN 2 bytes from `0x1d08` |
| `5` | OUT `0x1d00` (const `0d 8b …`), no read |
| `6` | OUT `0x9d00` (const `14 83 …`), OUT `0x1d08` 8 B, IN 2 B from `0x1d10` |
| `7` | writes `local_ab = 2` or `3` based on `this[0x22c] & 1`, then OUT `0x9d00` (`14 c3 …`), OUT `0x1d08` 8 B |
| default | logs an error |

**The full opcode table is no longer an open question.** What remains open is the *meaning* of
each opcode and the semantics of the parameter blocks — see
`09-open-questions-and-next-steps.md` §2.

---

## 4.3 Recovered command-layer functions

**[V]** C++ symbols in `libUVCCamera.so` (names recovered from the symbol table):

| Symbol | Meaning |
|--------|---------|
| `UVCPreviewIR::do_tinyC_order(JNIEnv*)` | entry point from Java |
| `UVCPreviewIR::doTinyCOrder()` | executes one order (the two-mode switch above) |
| `UVCPreviewIR::sendTinyCOrder(unsigned*, fn)` | generic order sender |
| `UVCPreviewIR::sendTinyCAllOrder(void*, fn, int)` | generic sender with opcode |
| `UVCPreviewIR::sendTinyCParamsModification(float*, fn, unsigned)` | write parameters |
| `UVCPreviewIR::getTinyCParams(void*, fn)` | read parameter block |
| `UVCPreviewIR::setTinySaveCameraParams()` | persist calibration to device |
| `UVCPreviewIR::setTinyCOutputADValue()` | output AD (raw) values |
| `UVCPreviewIR::getTinyCRobotSn(uint8_t*)` | read factory serial |
| `UVCPreviewIR::getTinyCUserSn(uint8_t*)` | read user serial |
| `UVCPreviewIR::getTinyCUserSnCoefficient(uint8_t*)` | read serial-derived coefficient |
| `UVCPreviewIR::getMachineSetting(int,int,int)` | read calibration setting |
| `UVCPreviewIR::setMachineSetting(int,int)` | write calibration setting |
| `UVCPreviewIR::tinyStartStream()` | start the "Tiny" stream |
| `UVCPreviewIR::tinyStartStream2()` | start variant 2 |
| `UVCPreviewIR::tinyStopStream()` | stop |

The names `Tiny1B` / `Tiny1C` (Windows `Tiny1BDll.dll`, `Tiny1CDll.dll`) and the Java
`nativetinyStartStream*` methods all refer to this same subsystem. **[V]**

**`setTinySaveCameraParams` is the function that writes calibration to the device.** A Linux
port must be able to *read* it before ever writing it. **[I]**

---

## 4.4 Preview / streaming

**[V]** `uvc_diy_start_preview` and `uvc_diy_stop_preview` are one-instruction tail-call
wrappers (`LDA`/`B`) with no recoverable body — their targets are other libuvc functions.
They most likely wrap `uvc_start_streaming` / `uvc_stop_streaming` with vendor defaults. **[I]**

What is established:

* Standard UVC streaming is used (`uvc_start_streaming`, `uvc_get_stream_ctrl_format_size`,
  `_uvc_populate_frame`, `_uvc_user_caller` callback thread, `uvc_allocate_frame`,
  `uvc_ensure_frame_size`). **[V]**
* The app supports multiple frame formats — `UVCCamera.getSupportedSize(int,int)` uses
  `mCurrentFrameFormat > 0 ? 6 : 4`, and `uvc_any2*` helpers exist for
  BGR / RGB / RGB565 / RGBX / YUYV / YUV420SP / IYUV420SP, plus `uvc_mjpeg2*`. **[V]**
* The thermal channel is a **second stream** or a second UVC interface: the product is
  "dual vision" and the Java layer has separate
  `handleStartPreview` (thermal) and `handleStartPreview_visible` (visible) paths. **[V]**
* Frame rate defaults to **30 fps** (`UVCPreviewIR+0x110 = 0x1e`) and the thermal payload format
  is now fully specified — see §4.5. The only residual unknown is which `bFormatIndex` carries the
  thermal stream, which needs `lsusb -v`. **[V]**/**[?]**

### Colour-conversion helpers present **[V]**

`uvc_any2bgr`, `uvc_any2rgb`, `uvc_any2rgb565`, `uvc_any2rgbx`, `uvc_any2yuyv`,
`uvc_any2yuv420SP`, `uvc_any2iyuv420SP`, `uvc_mjpeg2bgr`, `uvc_mjpeg2rgb`,
`uvc_mjpeg2rgb565`, `uvc_mjpeg2rgbx`, `uvc_mjpeg2yuyv`, `uvc_duplicate_frame`,
`uvc_ensure_frame_size`.

---

## 4.5 The thermal frame layout — **solved [V]**

> **This section used to be the only hard blocker to the Linux port.** It is now resolved
> **entirely by static analysis** — no USB capture and no hardware were required. Every claim
> below is cross-confirmed by at least two independent code paths, and the record layout was
> checked against all four supported sensor widths.

### 4.5.1 Geometry — the device appends 4 reference rows

The raw frame is a **flat, contiguous `uint16` array, row-major, `stride == width`**, holding
`width × (active_height + 4)` samples. The **last 4 rows are a reference / shutter band**
(the code calls it the `userArea`) — they are **not** image data.

The `+4` is proven three independent ways:

1. **Allocation.** `uvc_allocate_ini_frame(0x36c00)` sets width `0x180` (384) and height `0x124`
   (292); `384 × 292 × 2 = 224 256 = 0x36C00`. The 384 sensor is 384×288, so 4 extra rows are
   carried. The `else` branch is the 240×180 sensor with height `0xb8` (184) = 180 + 4.
   — `libuvc.so` `uvc_allocate_ini_frame @ 0x10ab24`
2. **Thermometry indexing.** `thermometrySearch` computes `n = (height - 4) * width` and iterates
   the image over exactly `n` samples. — `libthermometry.so` `thermometrySearch @ 0x101bf0`
3. **The `userArea` address.** `UVCPreviewIR::do_preview` locates the `userArea` at `raw + 0x18000`
   for a 256-wide frame. `0x18000 = 98 304 = 256 × 192 × 2`, exactly the byte offset at which the
   last 4 rows begin in a 256×196 frame. — `libUVCCamera.so` `do_preview`, ~line 26706

Resulting geometries **[V]**:

| Sensor (active) | Buffer rows | Buffer bytes | `userArea` byte offset | `thermometryT4Line` width key |
|---|---|---|---|---|
| 240 × 180 | 184 | `0x15900` | `0x15180` | `0xf0` |
| 256 × 192 | 196 | `0x18800` | `0x18000` | `0x100` |
| 384 × 288 | 292 | `0x36C00` | `0x36000` | `0x180` |
| 640 × 476 | 480 | `0x96000` **[I]** | `0x8F000` | `0x280` |

> The 640 entry is inferred from `UVCPreviewIR`'s **default** fields (width `0x280`, height
> `0x1e0`, buffer `0x96000`), which are placeholders overwritten per device by
> `FrameImage::setSize`. Treat 640×476 as unconfirmed. **[I]**

Key structure offsets **[V]**:

| Object | Field |
|---|---|
| `UVCPreviewIR+0xf8` | width |
| `UVCPreviewIR+0xfc` | **total** height (active + 4) |
| `UVCPreviewIR+0x198` | **the raw frame buffer** (passed to `do_temperature_callback` as `param_1[0x34]`) |
| `UVCPreviewIR+0x190` | second `w*h*2` buffer (copy target for "save picture") |
| `UVCPreviewIR+0x200` | `w*(h/2)*2` |
| `UVCPreviewIR+0x1c0` | `w*h*8` (float scratch) |
| `FrameImage+0x150` / `+0x154` | width / total height (`FrameImage::setSize`) |
| `FrameImage+0x550` | **16384-entry `float` LUT** (`0x10000` bytes) |
| `FrameImage+0x1054c` | `lut[16383]` — used as a "curve is flat/invalid" sentinel |
| `FrameImage+0x10550` | ambient temperature (°C) |
| `FrameImage+0x1055c` / `+0x10560` / `+0x10564` | Refltmp / Airtmp / humidity |
| `FrameImage+0x1056c` | sensor mode selector (`0x44` or `0x82`), default `0x82` |
| `FrameImage+0x10574` | `distance`, default `400` |
| `FrameImage+0x54c` | **"LUT dirty" flag** — `1` on construction, so the LUT is built on the first frame |
| `FrameImage+0x52d` | "recompute min/max this frame" flag |

### 4.5.2 The `userArea` calibration record

Within the 4 reference rows the device embeds a **per-unit calibration record**. Its start is at a
width-dependent offset from the `userArea` base (`rec_base`), and all other fields are at fixed
offsets from that start:

| width | `rec_base` | equals |
|---|---|---|
| `0xf0` (240) | `0x1e0` | 1 row |
| `0x100` (256) | `0x200` | 1 row |
| `0x180` (384) | `0x900` | 3 rows |
| `0x280` (640) | `0xf00` | 3 rows |
| other | `0x0` | — |

Record layout **[V]** (offsets from `userArea + rec_base`):

| offset | type | meaning | consumer |
|---|---|---|---|
| `+0x00` | `u16` | lower bound of the LUT sweep | `thermometryT4Line` `uVar4` |
| `+0x02` | `u16` | reference ADC value | `thermometryT4Line` `uVar3` |
| `+0x06` | `f32` | quadratic coefficient | `fVar1` |
| `+0x0a` | `f32` | linear coefficient | `fVar34` |
| `+0x0e` | `f32` | quadratic coefficient | `fVar27` |
| `+0x12` | `f32` | linear coefficient | `fVar2` |
| `+0x16` | `f32` | constant term | `fVar33` |
| `+0x40` | `u8[32]` | **serial-number key/IV blob** → `UVCPreviewIR+0xaf8` | `DecryptSNE` |
| `+0xfe` | `f32` | `correction` | `*param_7` |
| `+0x102` | `f32` | `Refltmp` (reflected temperature) | `*param_8` |
| `+0x106` | `f32` | `Airtmp` (air temperature) | `*param_9` |
| `+0x10a` | `f32` | `humi` (humidity) | `*param_10` |
| `+0x10e` | `f32` | `emiss` (emissivity) | `*param_11` |
| `+0x112` | `u16` | `userArea[0]` | logged only |
| `+0x114` | `u16` | `userArea[1]` | logged only |
| `+0x162` | `u8[]` | **encrypted serial number**, length `UVCPreviewIR+0xaf4` → `+0xb18` | `DecryptSNE` |

> **This layout is verified across widths.** For 256 the record is at `+0x200` and for 384 at
> `+0x900`, yet `+0xfe`, `+0x40`, `+0x112` and `+0x162` land on the same fields in both — checked
> against the raw `memcpy` offsets in `do_preview` and the `lVar*` tables in `thermometryT4Line`.
> The `+0x40` (key) and `+0x162` (ciphertext) offsets are what make the per-unit serial check
> self-contained in the frame. **[V]**

Separately, `thermometryT4Line` reads `userArea[1]` (the **2nd `u16` of the reference band,
absolute — not relative to `rec_base`**) as the ambient ADC sample, and derives the ambient
temperature with a width-specific affine law:

```c
/* u = userArea[1] (absolute u16 at raw + width*active_height*2 + 2) */
width 0x100 : T_amb = 20.0f - (float)(u - 0x21a9) / K1;
width 0xf0  : T_amb = 20.0f - (float)(u - 0x1e78) / K2;
width 0x180 : T_amb = 20.0f - (float)(u - 0x1e78) / K2;
width 0x280 : T_amb = 20.0f - (float)(u - 0x1ad3) / K3;
```

where `K1..K3` are the `f32` constants at `DAT_00101bc8` / `DAT_00101bc4` / `DAT_00101bcc` in
`libthermometry.so`. **This is a fully recovered formula and a useful cross-check on the port: the
reference-band sample must convert to a sane ambient temperature.** **[V]**

### 4.5.3 The output temperature array

`FrameImage::do_temperature_callback` (`@ 0x181ff8`) allocates a `jfloatArray` of
**`width * (height - 4) + 10`** floats:

| index | content |
|---|---|
| `[0]` | ambient / reference temperature (°C) |
| `[1]`, `[2]` | x, y of the **maximum** |
| `[3]` | maximum temperature (°C) |
| `[4]`, `[5]` | x, y of the **minimum** |
| `[6]` | minimum temperature (°C) |
| `[7]` … `[9]` | reserved, always `0.0` |
| `[10]` … `[10 + w*(h-4))` | **the temperature image**, row-major, `w` wide × `(h-4)` tall |

So the **produceable thermal image is `width × (height − 4)`** — for the 256×192 sensor that is
exactly 256×192. The 4 reference rows never appear in the output. **[V]**

The per-pixel law from `thermometrySearch` is a plain LUT lookup plus a per-frame offset:

```c
/* raw = uint16 frame, lut = 16384-entry float curve, n = width*(height-4) */
float corr = lut[ /* reference-band sample, see §4.5.2 */ ];
out[0] = lut[ raw[n + 12] ] + corr;
for (k = 0; k < n; k++)
    out[10 + k] = lut[ raw[k] ] + corr;
```

with a hard validity gate: **if any `raw[k] >= 0x4000` (16384) the function aborts** and prints
`"thermometrySearch err : 16383"`. A Linux port must reproduce this gate — it is how the device
signals a saturated or corrupt frame. **[V]**

The min/max coordinates in `[1]`, `[2]`, `[4]`, `[5]` are **image coordinates** and are mirrored
when the 180° rotation flag is set (`FrameImage::S0MaxMinRotate_180`:
`x' = width - x - 1`, `y' = height - y - 1`). **[V]**

### 4.5.4 The LUT and the two thermometry paths

`FrameImage+0x550` is a **16384-entry `float` LUT** — the radiometric response curve — regenerated
by `thermometryT4Line` whenever the `LUT dirty` flag `FrameImage+0x54c` is set. The code validates
it with `lut[0] < 1.0` (uninitialised) and `lut[0] == lut[16383]` (flat → invalid), and rebuilds
rather than emitting a frame when either holds. **[V]**

Two independent thermometry paths exist, selected by the device mode (§4.10):

**Mode `0x44c` (1100) — full radiometric.**
`thermometryT4Line` builds the LUT from the record coefficients and the reference band;
`thermometrySearch` then converts `w × (h−4)` pixels as in §4.5.3. Minimum/maximum are found by a
binary search over the AD range (`FrameImage::getDichotomySearch(..., 0x4000, 0, 0x3fff)`).

**Mode `1000` / `0x3eb` (1003) — direct AD conversion.**
No reference band and no LUT: the output is `w × h` floats computed as

```c
T_celsius = (float)raw / 64.0f - 273.15f;
```

i.e. the raw sample is **Kelvin × 64 (Q6 fixed point)**. This is
`FrameImage::adValueArray2FloatTempArray` (`@ 0x182978`), also inlined into
`do_temperature_callback`'s `mode == 1000` branch. It uses the *separate* dimension fields
`FrameImage+0x53c` / `+0x540`, not `+0x150` / `+0x154`. **[V]**

> **Porting consequence:** the simple path is trivially reproducible and makes an excellent
> bring-up target — if a device reports mode `1000`, `raw/64 - 273.15` is the entire thermometry.

> **Correction (2026-09-24, live device).** "the entire thermometry" is right; "the entire bring-up"
> is not. A mode-`1000` device does **not** emit real data until the host sends
> `setTinyCOutputADValue` *after* the UVC stream is running. Until then it streams a flat `0x8000`
> placeholder that decodes to `238.85 C`. See §4.10's correction for the full sequence.

### 4.5.5 Auto-shutter (flat-field) trigger — **must be replicated**

`do_preview` monitors the **second sample of the reference band** and fires a shutter command when
it drifts past a threshold:

```c
if (mode == 0x44c) {
    int32_t cur = raw[ width * (height - 4) + 1 ];
    if (last == 0) last = cur;
    if (abs(cur - last) >= this[0x140e84]) {      /* default 15 */
        uvc_set_zoom_abs(cam, 0xffff8000);        /* -> close shutter / re-reference */
        last = cur;
        this[0x281cf] = 0;
    }
}
```

`this+0x140e84` is initialised to `0xf` (15) in the constructor. **This is the device's automatic
flat-field correction (FFC).** The reference sample drifts with sensor temperature; when it moves
past the threshold the host must command a shutter. A Linux port that ignores this will develop
drift and banding within minutes of warm-up. The trigger is a standard UVC control write
(`uvc_set_zoom_abs(handle, 0xffff8000)`) — no vendor control transfer is involved. **[V]**

### 4.5.6 Frame rate and preview buffers

`UVCPreviewIR` constructor defaults **[V]**:

| field | value |
|---|---|
| `+0xf8` width | `0x280` (640) |
| `+0xfc` height | `0x1e0` (480) |
| `+0x110` fps | `0x1e` (30) |
| `+0x114` emissivity | `0x3f800000` (`1.0f`) |
| `+0x108` / `+0x10c` | `1` |
| `+0x1d0` (raw buffer) | `0x96000` |

`uvc_allocate_ini_preview_frame(0x6c000)` → width `0x180` (384), height `0x120` (288), tag `8`;
`0x6c000 = 384 × 288 × 4`, i.e. an **RGBA8888 preview frame**. `uvc_allocate_ini_frame` sets the
same `+0x20` tag to `3` for the 16-bit raw frame. **[V]**

`FrameImage`'s constructor builds four `cv::Mat`s, all type `0x18` = **`CV_8UC4`** (not `CV_8UC3`
as an earlier draft of this document stated): **[V]**

| expression | target | meaning |
|---|---|---|
| `cv::Mat(h, w*2, CV_8UC4)` | `+0x728` | **side-by-side dual image** (thermal ‖ visible) |
| `cv::Mat(h/2, w, CV_8UC4)` | `+0x8d8` | half-height |
| `cv::Mat(Size(*(cv::Size*)(this+0x8d0)), CV_8UC4)` | `+0x938`, `+0x3c8` | size from a field |

### Still not established **[?]**

These are the **only** remaining frame-level unknowns, and all three need `lsusb -v` on the real
device (they cannot be resolved statically):

* The UVC frame format GUID / `bFormatIndex` for the thermal stream.
* How the visible and thermal streams are demultiplexed at the UVC level — two interfaces, or two
  `bFormatIndex` values on one interface.
* The meaning of the `+0x20` "format tag" in the frame struct (`3` for the 16-bit raw frame,
  `8` for the RGBA preview frame).

> Everything needed to *parse* a frame is now known. What remains is only *how to subscribe* to
> the right stream, which one `lsusb -v` answers in seconds.

> **Resolved (2026-09-24, live device).** `lsusb -v` and `probe --descriptors` on the `0bda:5840`
> unit answer all three, so this section is closed:
>
> * **Thermal stream = `bFormatIndex 1`** — the *only* format on interface 1: `UNCOMPRESSED`
>   (`YUY2`, 16 bpp), with two frame descriptors, **256×192** (`bFrameIndex 1`, the default) and
>   256×384 (`bFrameIndex 2`). There is no second, visible format.
> * **No visible/thermal demux.** One VideoStreaming interface, one format; the "dual vision" is an
>   app-side composite. The vendor runs it on **`bAlternateSetting 7`** — probe/commit is
>   `bFormatIndex 1`, `bFrameIndex 1`, `dwFrameInterval 400000` (25 fps),
>   `dwMaxVideoFrameSize 98304`, `dwMaxPayloadTransferSize 3072`.
> * **`bFrameIndex 2` (256×384) is a decoy — always select frame 1.** Captured live with AD output
>   enabled, its whole 196,608-byte payload is synthetic filler: **one distinct row** (all 384 rows
>   byte-identical) and only 3 distinct values in a strict 4-pixel cycle. The AD pipeline emits
>   256×192 (98,304 B); asking for 384 gets the 192-row image padded with a pattern.
> * The `+0x20` format tag stays a host-side struct field; it does not affect subscribing.

---

## 4.6 Minimal Linux proof-of-concept sketch

This is **not yet tested** — it is the shape the port should take, given §4.2.

```c
/* Link against upstream libuvc + libusb-1.0. */
static int diy(uvc_device_handle_t *devh, uint8_t bmReq, uint8_t bReq,
               uint16_t wValue, uint16_t wIndex, uint8_t *buf,
               uint16_t len, unsigned timeout)
{
    int r = libusb_control_transfer(uvc_get_libusb_handle(devh),
                                    bmReq, bReq, wValue, wIndex, buf, len, timeout);
    if (r == 8) r = 0;                 /* match vendor normalisation */
    return r;
}

/* One command transaction. */
static int send_order(uvc_device_handle_t *devh, const uint8_t cmd[8],
                      uint8_t out[15])
{
    uint8_t status;
    int i;
    if (diy(devh, 0x41, 0x45, 0x78, 0x1d00, (uint8_t *)cmd, 8, 1000) != 0)
        return -1;
    for (i = 0; i < 1000; i++) {
        if (diy(devh, 0xC1, 0x44, 0x78, 0x0200, &status, 1, 1000) != 1)
            return -1;
        if ((status & 0x01) == 0) {
            if ((status & 0x02) == 0) break;
            if ((status & 0xfc) != 0) return -1;
        }
    }
    if (i == 1000) return -1;
    return diy(devh, 0xC1, 0x44, 0x78, 0x1d08, out, 15, 1000) == 15 ? 0 : -1;
}
```

Then the order sequence needed to bring the thermal stream up must be lifted from
`UVCPreviewIR::do_tinyC_order` / `tinyStartStream` / `tinyStartStream2`. **[?]**

> **Resolved (2026-09-24, live device).** The sequence is much shorter than this line assumed, and
> it does not involve `tinyStartStream` / `tinyStartStream2` at all — the vendor's startup sends
> exactly **one** order, `setTinyCOutputADValue`, *after* the stream is running. Full sequence and
> the latch to avoid are in §4.10's correction; the tested implementation is
> `linux-port/src/capture.c` (`send_ad_order`).

---

## 4.7 Device attach notes

* Android uses a **file-descriptor based** attach
  (`uvc_get_device_with_fd`, `nativeConnect(..., String)`) because the Android USB host API
  hands the app an fd. On Linux this is unnecessary — open the device normally with
  libusb. **[V]**
* The Windows build uses `UVCController.dll` + `libiruvc.dll`, i.e. the same model through
  a different libusb. **[V]**
* Device matching should use the VID/PID list in `01-target-and-hardware.md` §1.3. The list is now
  **fully decoded**: `UVCCamera::connect` maps each VID/PID to a device mode (§4.10), and the
  mode determines the whole thermometry path. `lsusb -v` is still needed on arrival, but only to
  pick which row of the §4.10 table applies to this unit and to find the thermal
  `bFormatIndex` — not to discover the protocol. **[V]**/**[?]**

---

## 4.8 Read/write safety classification

**[V]** Every recovered function falls into one of three classes. **A Linux port should implement
only the first class until the parameter block semantics are fully understood.**

### Safe — reads only

| Function | Transfers |
|---|---|
| `getTinyCRobotSn` `@ 0x168cc0` | OUT `0x1d00`, poll, IN 15 B `0x1d08` |
| `getTinyCUserSn` `@ 0x168a60` | poll, IN 15 B `0x1d08` |
| `getTinyCUserSnCoefficient` `@ 0x168800` | poll, IN **28 B** (`0x1c`) `0x1d08` |
| `getTinyCParams` `@ 0x167338` | OUT `0x9d00` (`14 85 00 03 …`), read back |
| `getTinyCUserData` `@ 0x167a24` | OUT `0x1d00` (`0d c1 …`) |
| `getTinyCDevicesStatus` `@ 0x168408` | status read |

### Stream control — writes device *mode*, not calibration

| Function | Transfers |
|---|---|
| `tinyStartStream` `@ 0x168ea8` | OUT `0x9d00` ← `0f c1 00 00 00 00 00 09` |
| `tinyStartStream2` `@ 0x168f9c` | OUT `0x1d08` ← 9 bytes `00 00 01 00 01 80 19 00 02` |
| `tinyStopStream` `@ 0x169090` | stop |
| `setTinyCOutputADValue` `@ 0x169d5c` | OUT `0x1d00` ← `0a 01 00 00 00 00 00 00` |

### **Dangerous — writes persistent device calibration**

| Function | Risk |
|---|---|
| `setTinySaveCameraParams` `@ 0x1712d4` | **saves** camera parameters to the device |
| `setMachineSetting` `@ 0x167e3c` | writes a calibration coefficient (§4.2) |
| `sendTinyCParamsModification` `@ 0x167760` | writes a parameter block |

> **These write to non-volatile device state.** A wrong write may destroy the unit's factory
> calibration, which **cannot be reconstructed** from the artifacts in this repository — the
> Windows calibration tables (`tau_*.bin`, `MILI6_*.bin`) are *not* confirmed to be the same data.
> Do not call them until the read path is fully understood and the exact parameter block layout
> has been verified against a known-good read. **[I]**

---

## 4.9 Serial-number decryption (`DecryptSNE`) — recovered

**[V]** `UVCPreviewIR::DecryptSNE(void *out, void *in15, char *robot_sn, void *out2)`
(`@ 0x1698bc`) is a short, fully-recovered byte deobfuscation routine. Ghidra split the working
array across several locals; reassembled, it is:

```c
/* b[0..14] is a 15-byte working buffer */
uint8_t b[15];
memcpy(b, in15, 15);

b[0] ^= 0x12;
for (i = 0; i < 15; i++) b[i] ^= 0x1d;

for (i = 0; i < 5; i += 2) {          /* swap pairs */
    uint8_t t = b[i]; b[i] = b[i+7]; b[i+7] = t;
}

for (i = 0; i < 7; i += 3) {
    if (i == 0) {                      /* mix in the tail */
        b[0] ^= b[12]; b[1] ^= b[13]; b[2] ^= b[14];
    } else if (i == 3) {
        b[4] ^= b[13]; b[5] ^= b[12];
    } else {                           /* i == 6: diffuse within the block */
        b[6] ^= b[9]; b[7] ^= b[10]; b[8] ^= b[11];
    }
}

memcpy(out, b, 15);
```

The decompiler also shows `atoi(robot_sn + 2) % 0x7f` being computed into a local that is never
subsequently read — it is either genuinely dead code or a Ghidra artefact of register reuse.
**[I]**

Note the constants `0x12` and `0x1d` and the `0x1d00`/`0x1d08`/`0x9d00` register indices — the
vendor reuses `0x1d` throughout, which is worth remembering when reading the disassembly.

---

## 4.10 Device mode selection — VID/PID → mode **[V]**

`UVCCamera::connect(int vid, int pid, int fd, int busNum, int devAddr, String usbfs)`
(`@ 0x15c28c`) — note the standard saki4510t/libuvc Android signature, so the first two
parameters are unambiguously **VID then PID** — selects the device mode:

```c
if      (vid == 0x1514 && pid == 0x0001)                       mode = 0x44c;  /* 1100 */
else if ((vid == 0x0bda && pid == 0x5840) ||
         (vid == 0x0bda && pid == 0x5830))                     mode = 1000;
else if ((vid == 0x0bda && pid == 0x5846) ||
         (vid == 0x0bda && pid == 0x31da) ||
         (vid == 0x0581 && pid == 0x0b00))                     mode = 0x3eb;  /* 1003 */
else                                                           mode = 0;
```

| VID:PID | Mode | Thermometry path | Sensor | `device_filter.xml` match |
|---|---|---|---|---|
| `1514:0001` | **`0x44C` (1100)** | full radiometric, 4 reference rows, `w×(h−4)` | 256×192 class | yes |
| `0BDA:5840` | **`1000`** | direct `raw/64 − 273.15`, `w×h` | — | yes |
| `0BDA:5830` | **`1000`** | direct `raw/64 − 273.15`, `w×h` | — | yes |
| `0BDA:5846` | **`0x3EB` (1003)** | third variant | — | yes |
| `0BDA:31DA` | **`0x3EB` (1003)** | third variant | — | *(see note)* |
| `0581:0B00` | **`0x3EB` (1003)** | third variant | — | yes |
| anything else | `0` | unsupported — app refuses | — | — |

> **Discrepancy worth knowing.** The APK's `device_filter.xml` lists `31DA:5846` (VID `0x31DA`,
> PID `0x5846`), but `connect` tests `VID 0x5846 / PID 0x31DA` — the **transpose**. Four of the
> five XML entries match `connect` exactly; this one does not. Either the XML has a typo or the
> device advertises both orderings. It does not affect the port (the mode is what matters), but
> it does mean the XML list alone is not a reliable authority. **[V]**

### Why this matters for the port

* **`mode` selects the entire thermometry path.** `UVCPreviewIR::do_temperature_callback`
  (`@ 0x1725d8`) and `do_preview` both branch on `*(int*)(this+0x8c)`, and
  `FrameImage::do_temperature_callback` branches again on its own copy. A Linux port must
  read the VID/PID, derive the same mode, and dispatch identically.
* **`mode == 0x44c` is the radiometric one.** The 4-reference-row layout of §4.5.1, the
  `userArea` record of §4.5.2, the auto-shutter trigger of §4.5.5 and the serial-number check
  all live behind this mode.
* **The 256×192 unit described in the manuals is almost certainly `1514:0001` → mode `0x44C`.**
  Reasoning: `thermometryT4Line` has a dedicated calibration offset table for width `0x100`
  (256), and the `do_preview` `userArea` lookup's `else` branch uses `raw + 0x18000`, which is
  exactly `256 × 192 × 2`. Both are the mode-`0x44c` code path. **[I]** — confirm with `lsusb`
  on arrival.
* **`mode` is also what gates the serial-number check.** `isVerifySN` and the
  `configs_maintenanceguy.txt` allow-list comparison (§4.9 and `03-android-app-architecture.md`
  §3.7) only run for the radiometric modes; if the SN does not verify, the preview is suppressed
  (`this+0xaf1` stays `0`, `this+0x15e` is set). **A Linux port must therefore expect the device
  to withhold frames until the serial handshake succeeds.** **[V]**

> **Bring-up order for the port:** identify VID/PID → derive mode → if `0x44c`, complete the SN
> handshake before expecting thermal frames; if `1000`, frames flow immediately and thermometry
> is `raw/64 − 273.15`.

> **Correction (2026-09-24, live device) — this unit is `0bda:5840` / mode `1000`, and mode `1000`
> does *not* flow frames immediately.**
>
> The 256×192 camera in this repository enumerates as **`0bda:5840`** (Realtek "USB Camera",
> manufacturer `Generic`, serial `200901010001`) — **not** the `1514:0001` inferred above — so its
> mode is **`1000` (direct AD)**, not `0x44c`. The reasoning above was wrong: the 256-wide
> calibration table in `thermometryT4Line` and the `256 × 192 × 2` reference-band offset in
> `do_preview` are both mode-`0x44c` *code* paths, but they do not imply that a 256×192 *device* is
> `0x44c`. `probe --descriptors` and `lsusb -v` agree on the VID/PID.
>
> The bring-up is also longer than "frames flow immediately". Verified against
> `MechaniscoutPcap/4.pcapng` (a capture that starts *before* the Windows app connects) and then
> reproduced on Linux:
>
> 1. Enumerate; negotiate UVC on interface 1 and `SET_INTERFACE` alt 7.
> 2. The stream runs but delivers a **flat `0x8000` placeholder** (`238.85 C` under
>    `raw/64 − 273.15`) — every sample identical, no sensor noise.
> 3. The host sends **one** order — `setTinyCOutputADValue`,
>    `0x41/0x45 wValue=0x0078 wIndex=0x1D00  0a 01 00 00 00 00 00 00`.
> 4. Real thermal data appears **~2–3 s later**, after a settle transient.
>
> **The ordering is the whole point.** Sent *before* the stream exists, that order makes the device
> return `0x01` and then **latch `0x0e` permanently** — every later command returns `0x0e`, and
> every `0x1d08` read returns stale bytes (this is the `0x1d08` "echo" of §4.2). The latch survives
> handle close/open; **only a USB replug clears it.** `tinyStartStream`, `tinyStartStream2` and
> `getTinyCParams` do **not** appear in the vendor's startup at all — sending them is what poisoned
> earlier sessions. The port now sends only `setTinyCOutputADValue`, after streaming starts
> (`linux-port/src/capture.c:send_ad_order`, `capture_demo --start-orders`).
