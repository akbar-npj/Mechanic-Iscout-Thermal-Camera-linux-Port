# 03 — Android Application Architecture

**Package:** `com.dyt.wcc.maintenanceguy` · **versionName** `3.04.07` (code 571) ·
minSdk 26 / targetSdk 34 · `extractNativeLibs="false"` **[V]** (`jadx/resources/AndroidManifest.xml`)

`android:name="com.dyt.wcc.constans.DYTApplication"` — the `Application` subclass.

---

## 3.1 APK layout

```
META-INF/com/android/build/gradle/app-metadata.properties
classes.dex              6.3 MB
classes2.dex             5.1 MB
resources.arsc           1.2 MB
AndroidManifest.xml
assets/
    1.dat .. 6.dat       palettes, 768 B each      (DYConstants.paletteArrays)
    7.dat                palette,  768 B           (purpose not confirmed [?])
    configs_maintenanceguy.txt   3 base64 blobs, ';'-separated
    TYF0ReadMeEN.pdf / TYF0ReadMeCN.pdf            (18 / 19 pp)
    TYF0CReadMeEN.pdf / TYF0CReadMeCN.pdf          (28 / 29 pp)
    iScoutRead.pdf                                 (44 pp, 23.7 MB)
lib/arm64-v8a/  armeabi-v7a/  x86/  x86_64/        (4 ABIs)
```

1384 zip entries, 178 MB uncompressed. **[V]**

---

## 3.2 Java package map

```
com.dyt.wcc
├── constans/          DYConstants, DYTApplication, DYTRobotSingle
├── cameracommon/      BuildConfig, R
│   ├── usbcameracommon/   AbstractUVCCameraHandler, UVCCameraHandler, CaptureData
│   ├── encoder/ entity/ factory/ utils/ widget/ callback/
├── common/            base/, utils/, widget/ (dragView, mySpinner)
├── customize/         per-OEM skins:
│     aixun dobiyen dobiycn dyt ht820 jms kepuruimt maintenanceguy maant
│     marmonix msl256i mti448 neutral tletek …
├── ui/
│   ├── preview/       MainActivity, PreviewFragment, record/
│   └── gallery/       galleryDetail/
├── utils/             ByteUtilsCC, …
└── maintenanceguy/    build-specific resources

com.serenegiant.usb     UVCCamera, USBMonitor, USBVendorId  (vendored upstream libuvc JNI wrapper)
com.huantansheng.easyphotos   photo picker
com.hjq.permissions     runtime permissions
com.bumptech.glide, com.davemorrissey, okhttp3, kotlin, org.*   third-party
```

**[V]** (`find` over `RE Workspace/jadx/sources`)

The OEM skins are the key architectural fact: **one camera/thermometry core, many
brandings.** For a Linux port, ignore `customize/` entirely.

---

## 3.3 Native library dependency graph

**[V]** (`readelf -dW` on every `lib/arm64-v8a/*.so`)

```
                       libc++_shared.so ──┐
                                          │
libUVCCamera.so  ──┬── libuvc.so ─────────┤
  (the only JNI    │      └── libusb100.so
   entry point)    ├── libthermometry.so  │   <-- pure C, no JNI, no libc++
                   ├── libsimplePictureProcessing.so
                   ├── libDYTJpegAes.so
                   ├── libjpeg-turbo1500.so
                   ├── libopencv_java4.so ── libmediandk.so, libjnigraphics.so, libz.so
                   └── liblog.so, libandroid.so

libmnnmodel.so ── libMNN.so, libMNN_Express.so, libc++_shared.so
libPhotoNativeHelper.so  (standalone; used by easyphotos for file verification)
```

### Why this matters for the port

* **`libUVCCamera.so` is the whole story.** It is the *single* JNI library and links
  everything. It is also the only library that depends on Android-specific libs
  (`libandroid.so`, `liblog.so`, `libjnigraphics.so`, `libmediandk.so`) — i.e. it is the
  only thing that must be replaced on Linux. **[V]**
* `libthermometry.so` and `libuvc.so` are **plain C**, no JNI, and link only
  `libc/libm/libdl/liblog` (+ `libstdc++` for thermometry). They are the two pieces worth
  reimplementing (or reusing) on Linux. **[V]**
* `libDYTJpegAes.so` depends only on `libc/libm/libdl/liblog` — also portable. **[V]**
* `libMNN*` is Alibaba MNN (neural inference). Its role is now **established** **[V]** (2026-09-25):
  `libmnnmodel.so` embeds a 2× super-resolution model whose weights are XOR-obfuscated in its own
  `.data` (`09-…` §6). It is still *not* on the critical path for basic capture, and the Linux port
  implements the same capability against an upstream MNN build rather than these bionic libraries
  (`08-…` §8.11).

---

## 3.4 JNI surface

`libUVCCamera.so` exports **zero** `Java_*` symbols. **[V]** (`nm -D --defined-only | grep -c Java_` → 0).
JNI is bound dynamically in `JNI_OnLoad` (`@ 0x5b8b0`). **[V]**

The Java-side declarations in `com/serenegiant/usb/UVCCamera.java` are therefore the
authoritative method list. The camera-relevant ones:

| Native method | Purpose |
|---------------|---------|
| `nativeCreate()` / `nativeDestroy(long)` / `nativeRelease(long)` | lifecycle |
| `nativeConnect(long, int, int, int, int, int, String)` | open device (fd + descriptors) |
| `nativeStartPreview` / `nativeStopPreview` | (via `AbstractUVCCameraHandler`) |
| `nativeGetByteArrayTemperaturePara(long, int)` | **thermometry parameter blob** (128 B) |
| `nativeGetCameraParams(long, int)` | camera parameters |
| `nativeGetByteArrayPicture(long, byte[])` | frame grab |
| `nativeGetMachineSetting(long,int,int,int)` / `nativeSetMachineSetting` | calibration knobs |
| `nativeJavaSendJniOrder(long, int)` | send a command to the device |
| `nativeCapturePhoto` / `nativeSavePreViewPicture` / `nativeReadPicture` | capture |
| `nativeChangePalette` / `nativeGetSupportedSize` / `nativeIsSuperResolution` | image opts |
| `nativeRenderTempRangeChange` / `nativeLockRenderTempRange` | display range |
| `nativemyPattern(int)` / `nativemyCoefficient(int,int)` / `nativeSetAdjustedValue(float)` | fusion/visible-overlay tuning |
| `nativetinyStartStream` / `nativetinyStartStream2` / `nativetinyStopStream` | **TinyC stream control** |
| `nativeWhenChangeTempPara` / `nativeWhenShutRefresh` | parameter-change hooks |

> Note the naming split: `Tiny1B`/`Tiny1C` (Windows `Tiny1BDll.dll`, `Tiny1CDll.dll`) and the
> `tiny*` Java natives point at a **"Tiny" command family**. The Windows `libircmd.dll` +
> `sendTinyCAllOrder` symbol (see `04-usb-protocol.md`) is the same subsystem. **[V]**

---

## 3.5 Command dispatch (camera thread message codes)

**[V]** `com/dyt/wcc/cameracommon/usbcameracommon/AbstractUVCCameraHandler.java` `handleMessage`.
This is a complete map of every camera operation the app performs:

| `what` | Handler | Meaning |
|--------|---------|---------|
| 0 | `handleOpen(UsbControlBlock)` | open device |
| 1 | `handleClose()` | close |
| 2 | `handleStartPreview(obj)` | start preview |
| 3 | `handleStopPreview()` | stop preview |
| 4 | `handleCaptureStill(CaptureData)` | still capture |
| 5 | `handleStartRecording(bool)` | start video |
| 6 | `handleStopRecording()` | stop video |
| 7 | `handleUpdateMedia(String)` | media scan |
| 8 | `handlePreparePalette(String, int)` | load palette file |
| 9 | `handleRelease()` | release |
| 10 / 11 | `handleStartTemperaturing()` / `handleStopTemperaturing()` | **thermometry on/off** |
| 13 | `handleChangePalette(int)` | switch palette |
| 14 | `handleSetTempRange(int)` | temp range |
| 15 | `handleMakeReport()` | generate .docx report |
| 16 / 17 | `handleOpenSysCamera()` / `handleCloseSysCamera()` | visible camera |
| 18 / 19 | `handleSetHighThrow(int)` / `handleSetLowThrow(int)` | alarm thresholds |
| 20 / 21 | `handleSetHighPlat(int)` / `handleSetLowPlat(int)` | display plateaus |
| 22 / 23 | `handleSetOrgSubGsHigh(int)` / `handleSetOrgSubGsLow(int)` | org/sub-grayscale |
| 24 / 25 | `handleSetSigmaD(float)` / `handleSetSigmaR(float)` | sigma (÷10) |
| 26 | `handleRelayout(int)` | UI |
| 30 | `handleSetPalette(int)` | palette index |
| 31 / 32 | `handleRenderTempRangeChange(f,f,f,f)` / `handleRenderTempRangeDisable()` | |
| 33 / 34 | `handleSetAreaCheck(int)` / `handleSetArea(obj)` | region measurement |
| 35 / 36 | `handleTempShowOnOff(bool)` / `handleFixedTempStrip(bool)` | |
| 40 | `handleSaveFiveSeconds(String)` | |
| 45 | `handleTinySaveCameraParams()` | **persist calibration to device** |
| 50 | `handleSavePicture(CaptureData)` | |
| 55 | `setVerifySn()` | **serial-number verification** |
| 56 | `handleStartPreview_visible()` | visible stream |
| 57 | `myPattern(int)` | fusion pattern |
| 58 | `myCoefficient(int,int)` | X/Y fusion coefficients |
| 59 | `setAdjustedValue(float)` | correction |
| 65 | `IsSuperResolution(bool)` | super-resolution |

**This table is the functional specification of the Linux port.** Any Linux application
that implements `open / start preview / start thermometry / read frame / read temperature /
set palette / capture` covers the core value of the product.

---

## 3.5.1 Grayscale (visible-image) mapping — recovered from the OpenCL kernels

**[V]** The rows above for `what` 20/21/24/25 ("display plateaus", "sigma") previously had no
semantics recorded. They are now recovered. The implementation is **not** in Java:
`handleSetHighPlat`/`handleSetLowPlat`/`handleSetSigmaD`/`handleSetSigmaR` are **empty stubs**
in the decompiled APK (`AbstractUVCCameraHandler.java:1127-1152`), and no Java class references
`libsimplePictureProcessing` at all. The real work is in
`RE Workspace/apk/lib/arm64-v8a/libsimplePictureProcessing.so`, which `libUVCCamera.so`
`dlopen()`s **by name** (so the call sites are not statically visible) and which embeds two
OpenCL kernel sources as **plaintext strings** — `strings -a libsimplePictureProcessing.so`
prints them at the very top of the file. They are reproduced verbatim below (line breaks as in
the binary; the missing closing braces are an artefact of `strings` splitting on newlines).

### Kernel 1 — `bilateralBlur` (the detail layer)

```c
__kernel void bilateralBlur( __global const ushort* src, __global short* BilSubGs,
                             const int ksize, float sigma_d, float sigma_r,
                             const float alpha, const int width, const int height,
                             __global const uchar *gsPara, global short* OrgSubGs,
                             global short* BilOutPut, int OrgSubGsHigh, int OrgSubGsLow)
{
    int x = (int)get_global_id(0), y = (int)get_global_id(1);
    if (x >= width || y >= height) return;

    short fij = src[width*y + x];
    float numerator = 0.0f, denominator = 0.0f;
    int GsSum = 0;

    for (int K = -ksize/2; K <= ksize/2; K++) {
      for (int L = -ksize/2; L <= ksize/2; L++) {
        short fkl = ((y+L) < 0 || (x+K) < 0 ||
                     (y+L) >= height || (x+K) >= width)
                  ? src[width*y + x]                    /* clamp: edge repeats centre */
                  : src[width*(y+L) + x+K];

        float dkl = -(K*K + L*L) / (2 * sigma_d * sigma_d);
        float rkl = -(fij - fkl)*(fij - fkl) / (2 * sigma_r * sigma_r);
        float wkl = exp(dkl + rkl);

        numerator   += fkl * wkl;
        denominator += wkl;
        GsSum       += fkl * gsPara[(L+2)*5 + K+2];     /* fixed 5x5 Gaussian weights */
      }
    }

    float gij = (denominator != 0) ? numerator / denominator : 0.0f;
    short Bil = (short)gij;
    BilOutPut[width*y + x] = Bil;

    short Gs = (short)(GsSum >> 7);                     /* weights sum to 128 */

    int bfSubGs = Bil - Gs;                             /* the detail layer */
    if (bfSubGs < -30) bfSubGs = -30;
    if (bfSubGs >  30) bfSubGs =  30;
    BilSubGs[width*y + x] = bfSubGs;

    short sub = fij - Gs;
    if      (sub < OrgSubGsLow)  OrgSubGs[width*y + x] = OrgSubGsLow;
    else if (sub > OrgSubGsHigh) OrgSubGs[width*y + x] = OrgSubGsHigh;
    else                         OrgSubGs[width*y + x] = sub;
}
```

So the "sigma" pair is a **classic bilateral filter**: `sigma_d` is the *domain* (spatial)
sigma and `sigma_r` the *range* (intensity) sigma. `Gs` is a plain 5×5 Gaussian blur of the
same input (fixed-point weights summing to 128, hence the `>> 7`), and the **detail layer** is
`bilateral − gaussian`, hard-clamped to **±30**. `alpha` is declared but never read.

### Kernel 2 — `linearPlatKernel` (the stretch)

```c
__kernel void linearPlatKernel(__global const ushort* input, __global uchar4* output,
                               const int width, const int height,
                               const int Deta, const int min, const int max,
                               const int quantity,
                               __global const int *platAcc, const float linearPercent,
                               const float platPercent, __global const short *bilSubGs,
                               __global const char *palette1, const int kindOfPalette)
{
    for (int i = get_global_id(0); i < width*height; i += get_global_size(0)) {
        float linearOut, platOut;
        int temp, gray;

        if (input[i] > max) {
            linearOut = (max - (max+min)/2) * 128 / Deta + 128;   platOut = 255;
        } else if (input[i] < min) {
            linearOut = (min - (max+min)/2) * 128 / Deta + 128;   platOut = 0;
        } else {
            linearOut = (input[i] - (max+min)/2) * 128 / Deta + 128;
            platOut   = 255 * platAcc[input[i]] / quantity;       /* histogram CDF */
        }

        gray = (int)(linearOut*linearPercent + platOut*platPercent + bilSubGs[i]);
        /* then clamp + emit, per kindOfPalette (below) */
    }
}
```

`Deta = max − min`. `platAcc` is an **int cumulative histogram** indexed by the input sample
value and `quantity` is the total pixel count, so `platOut` is a **histogram-equalisation
term** in 0…255. `linearOut` is a linear stretch of `[min,max]` onto **[64,192]** (at
`input=min` it is exactly 64; at `max`, 192) — i.e. it deliberately uses only the middle 50 %
of the range, leaving headroom for the other two terms. The final value is

```
gray = clamp( linearPercent·linearOut  +  platPercent·platOut  +  detail , 0 , 255 )
```

`kindOfPalette` then selects the output: `0` = grey (`r=g=b=gray`), `1` = inverted grey
(`255−gray`), `3` = palette lookup at `gray·448/256` clamped to 447, `4` = palette lookup at
`gray·224/256` clamped to 220, `default` = palette lookup at `gray` clamped to 255. (Case `2`
is absent from the kernel.)

### Mapping onto the Java handler names

| Java handler | Kernel argument | Meaning |
|---|---|---|
| `handleSetSigmaD(float)` | `sigma_d` | bilateral **domain** sigma (sent as int ÷ 10) |
| `handleSetSigmaR(float)` | `sigma_r` | bilateral **range** sigma (sent as int ÷ 10) |
| `handleSetOrgSubGsHigh/Low(int)` | `OrgSubGsHigh/Low` | clamp bounds on `fij − Gs` |
| `handleSetHighPlat/LowPlat(int)` | → `min`/`max`/`Deta` | the **plateau**: high/low percentile bounds from which the input window is derived |
| `handleSetHighThrow/LowThrow(int)` | — | alarm thresholds (see §3.5 rows 18/19) |

`linearPercent`/`platPercent` (the linear↔equalisation blend) have no matching handler, so
they are presumed fixed at the call site in `libUVCCamera.so`.

### Not recovered (deliberately)

The **tuning constants** — `sigma_d`, `sigma_r`, `ksize`, the 25 `gsPara` weights, the
`OrgSubGs` clamps, the plateau percentiles and the blend weights — are runtime-settable and
are **not** statically recoverable: `libUVCCamera.so` reaches the library through `dlopen`/
`dlsym`, so the argument setup is not visible in its import table, and the Java side that
would supply them is stubbed out. The Linux port therefore implements the **recovered
structure** with documented defaults and keeps every constant configurable, which mirrors the
vendor's own API (these are user settings, not constants). See
`linux-port/src/display.h` (`dyt_gray_params_t`).

---

## 3.5.2 Image fusion (the six patterns) — recovered from `fusionFunction`

**[V]** for the pattern list and the four cases that are decodable; **[I]** for the two cases the
decompiler does not fully resolve. `myPattern(int)` (message 57) selects one of **six** fusion
patterns; `myCoefficient(int,int)` (message 58) sets the X/Y alignment.

**The six patterns.** The APK enumerates them as drawable resources and `setPattern` rejects
anything outside `0..5` (`PreviewFragment.java:1926-1935`: `if (… || i >= 6) return;`), so "six" is
**[V]**:

| idx | resource | name | native `+0x228` |
|---|---|---|---|
| 0 | `pattern_layout_infrared` | Infrared (thermal only) | 0 |
| 1 | `pattern_layout_visible_light` | Visible light | 1 |
| 2 | `pattern_layout_edge_blending` | Edge blending | 2 |
| 3 | `pattern_layout_degrees_of_fusion` | Degrees of fusion | 3 |
| 4 | `pattern_layout_picture_in_picturen` | Picture in picture | 4 |
| 5 | `pattern_layout_edge_blending_black` | Edge blending (black) | 5 |

**The native switch.** `UVCPreviewIR::fusionFunction` (`@ 0x16d4c8`, `libUVCCamera.so`) switches on
the pattern. It operates on two **RGBA Mats**: `+0x368` = visible, `+0x3c8` = thermal, output
`+0x428`. Decoded operands (the decompiler passes doubles in `xmm` registers, shown here as their
hex bit patterns):

| case | pipeline |
|---|---|
| **0** | not in the switch — `do_fusion` skips the fusion thread when the pattern is `< 2`, so 0 is thermal-only |
| **1** | not in the switch — visible-only path (`yuy2_rgba`, `copyToSurface`) |
| **2** | `cvtColor(visible, BGRA2GRAY, code 10)` → `bilateralFilter(d=7, σColor=11.0, σSpace=11.0, BORDER_REFLECT_101)` → `Sobel(ksize=3, dx=1, dy=0, scale=1)` → `convertScaleAbs` → **same for `dy=1`** → `add` the two magnitudes → `cvtColor(GRAY2BGRA)` → `add(thermal, edges)` |
| **3** | `addWeighted(visible, 0.5, thermal, 0.5, 0)` — a straight 50/50 blend |
| **4** | `Rect(200, 120, 240, 240)`; the visible frame is the base and the thermal ROI is pasted into that rectangle |
| **5** | the case-2 grey → bilateral → \|Sobel_x\|+\|Sobel_y\| chain, plus a **mask built from the *bilateral* plane**: `d1 = dilate(blur, K1)`, `d2 = dilate(d1, K2)` (K1/K2 live at `+0x870`/`+0x810` and are **not defined in this function**), `d = d2 − d1 + C` (C is a constant Mat at `+0x7b0`, also not defined here), `m = threshold(d, 70.0, 255.0, type=3)`, `mask = bitwise_not(m)`, then `out = bitwise_and(thermal + \|dx\|+\|dy\|, mask_as_image, mask=mask)` |

The `0x4026…` doubles are `11.0`, the `0x3ff0…` are `1.0`, `0x40518…` is `70.0` and `0x406fe…` is
`255.0`. `threshold` type `3` is `THRESH_TOZERO`.

**X/Y alignment [V].** `DYConstants.X_Coefficient` / `Y_Coefficient` default to **0** and are
clamped to **−40 … +40**; four on-screen arrows nudge each by ±1
(`PreviewFragment.java:1939-1960`). Native stores them at `+0x220` (X) and `+0x224` (Y)
(`UVCPreviewIR::myCoefficient @ 0x16f380`), and `yuy2_rgba` adds them to the origin of a 4-short
thermal ROI rect held at `+0xb70` inside the decoded **640×480** visible frame, clamping to
639/479.

> **Two caveats the port must carry, because they shape what "faithful" can mean.**
>
> 1. **The vendor fuses a *different* visible source.** `fusionFunction`'s visible Mat comes from
>    `handleStartPreview_visible` (message 56) — a **separate 640×480 MJPEG stream**
>    (`+0x1e0`/`+0x1f0`), decoded by `yuy2_rgba`. It is **not** the 256×192 grayscale top half of the
>    dual-half payload (RE Docs 04 §4.10). So the vendor's geometry (640×480 visible + a 240×240
>    thermal ROI) does not map onto the port's device-default frame; the port fuses the **top half**
>    with the thermal plane, which are both 256×192 and therefore already 1:1.
> 2. **Case 5's kernels are not statically recoverable.** The two `dilate` structuring elements live
>    at `+0x870`/`+0x810` and are built outside this function; their size/shape is `[?]`. As with
>    §3.5.1, the port implements the **recovered structure** with documented constants rather than
>    guessing them.

The six patterns' *shape* is therefore **[V]**, their exact pixel output at the port's geometry is
**[I]** — see `linux-port/src/fusion.h` for the constants the port uses and where it departs.

> **Consequence for the port's alignment [I]:** because the vendor's coefficient moves the *thermal*
> ROI inside the visible frame, the port applies it to the **visible plane before the edges are
> computed** — an edge that comes from the visible plane has to be sampled at the same offset as the
> rest of that plane, or the key would do nothing on the two edge patterns. Verified live
> 2026-09-25: `dx = +2` shifts the fused picture exactly 2 source px, measured by cross-correlation
> (`04-usb-protocol.md` §4.10).
>
> **Consequence for case 5 [V]:** its mask depends on **three unrecovered quantities** — the two
> `dilate` kernels and the constant Mat added at `+0x7b0` — and the bias is what sets the mode's
> whole character (with `C = 0` the threshold passes almost every pixel and the result collapses
> onto case 2). The port therefore does **not** invent them: it applies the one recovered constant,
> the `threshold(70)`, to a 3×3-dilated edge magnitude and treats the result as the keep-mask. That
> is a documented **simplification** of the dataflow, not the dataflow — the two are not equivalent,
> and the port's version renders almost entirely black on this unit's low-contrast visible picture
> (35 grey levels across the frozen fixture). See `linux-port/src/fusion.h`.

---

## 3.6 Thermometry parameter blob

**[V]** `AbstractUVCCameraHandler.handleMakeReport()` reads
`mUVCCamera.getByteArrayTemperaturePara(128)` — a **128-byte** blob — and parses:

```
offset  0  float  (little-endian)
offset  4  float
offset  8  float
offset 12  float
offset 16  float
offset 20  short
```

`PreviewFragment.java:3167` also calls
`ByteUtilsCC.byte2Float(mUvcCameraHandler.getTemperaturePara(128))`.
`ByteUtil.getFloat()` is plain little-endian `Float.intBitsToFloat`. **[V]**

The five floats + one short are the device-reported thermometry parameters. Their
individual meanings are **not yet established [?]** — they are most likely a subset of
`{emissivity, reflected temp, ambient temp, humidity, distance, correction}` because those
are exactly the six values the thermometry engine consumes (see
`05-thermometry-algorithm.md` §5.5). **Confirm against the parameter block offsets used in
`libthermometry`'s callers.**

---

## 3.7 Encrypted configuration

**[V]** `assets/configs_maintenanceguy.txt` (74 bytes):

```
yli6PhIDL6/rw8I2zJiiVQ==;O4CB46unY+T0DfC3e11NvA==;6G4s4lkzlHHP++idImyZCA==
```

Three base64 blobs of 16 bytes each (one AES block). **These are not branding strings — this is
a device serial-number authorisation allow-list.** Full trace and the recovered AES parameters are
in `06-asset-and-file-formats.md` §3.2-3.3; the short version:

* `AssetCopyer.copyAllAssets()` copies the asset to external storage.
* `PreviewFragment.java:2100` passes its path to native
  `PreparePalette(path, paletteIndex, "configs_maintenanceguy")`.
* `UVCPreviewIR::do_preview` (`libUVCCamera.so`) splits the file on `;`, AES-decrypts each entry,
  takes the first 8 bytes, and compares them against the camera's own serial number (itself
  decrypted from the frame buffer by `UVCPreviewIR::DecryptSNE`).
* On a match it logs `"verify sn success"` and sets the verified flag `this+0xaf1 = 1`.
* **[I]** AES-128-CBC with key = IV = `"dyt1101c"` NUL-padded to 16 bytes yields the serials
  `DYTEPK78`, `DYCRPK78`, `DYCRPK79` — consistent with the real serials `DYTCA09B`, `DYTCQ10Q`
  found elsewhere in the binaries.

The Windows build carries the same construct with more fields (`blk027.bin`):

```
[CONFIG]
Title=            Titleen=          Titlezh-Hant=
Mail=07U3UWZVCIgrRAFJu+mYMYSs7Wb2vOqX
Website=PEI3/goZW6d5JPmm/sZYlg==
Websiteen=PEI3/goZW6fmCVK4JoX8vCwNjQEtCRAy
Path=eeWPuDNh91JwQcRw8KCoZvjRVBF81mn9
Company=ETpKGF21GRYgVLuClkg/EuGQ8l18xyFoB2YCy1n1vV/PPAx8V2yBSA==
Companyen=BsOXzYnQxtptD2qJ/w/n7HGNB4MaegATQvaIblWHQ9AmNwvVDeftcy4xJqMyTJhT
Code=u3yoM46skJ0lYtLXgBo4Bg==
```

The AES + base64 primitives are exported by `libDYTJpegAes.so` **[V]**:

```
_ZN3AES13EncryptionAESERKNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE
_ZN3AES13DecryptionAESERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcENS0_9allocatorIcEEEE
_Z13base64_encodePKhj     _Z13base64_decodeRKNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE
_Z9DecryptSNPvS_
```

`_Z9DecryptSNPvS_` = `DecryptSN(void*, void*)` — a key/SN-derivation helper. **[V]** (symbol exists)

**[V]** The AES class hard-codes its key material. `AES::AES()` (`_ZN3AESC2Ev` @ `0x126908`)
copies two NUL-padded 16-byte strings into the object at `+0x478` and `+0x489`:

```
+0x478 : "dyt1101c"          (padded with 0x00 to 16 bytes)
+0x489 : "dyt0526cdyt0526c"
```

**[I]** key = IV = `"dyt1101c"` (NUL-padded) in AES-128-CBC decrypts the allow-list to plausible
8-byte serials. The alternative assignment (key `dyt1101c…`, IV `dyt0526cdyt0526c`) also yields
plausible serials (`DYTDTI08`, `DYCSTI08`, `DYCSTI09`); the key-vs-IV roles are not yet
disambiguated. Details and the differential test that corroborates the key are in
`06-asset-and-file-formats.md` §3.3, and it is listed in
`09-open-questions-and-next-steps.md` §5.

This is **not** needed for basic operation — it is a licence gate on device serial numbers, and a
Linux client we write ourselves has no reason to reproduce it.

---

## 3.8 Declared permissions and features

**[V]** `AndroidManifest.xml`:

`MANAGE_MEDIA`, `READ_MEDIA_*`, `READ/WRITE_EXTERNAL_STORAGE`, `RECORD_AUDIO`, `CAMERA`,
`MOUNT_UNMOUNT_FILESYSTEMS`; features `android.hardware.usb.host` (**required**),
`android.hardware.camera`, `android.hardware.camera.autofocus`, OpenGL ES 2.0.

Activities: `com.dyt.wcc.ui.preview.MainActivity` (exported, `sensorLandscape`),
`com.dyt.wcc.ui.preview.AlbumActivity`.

Note `android:usesCleartextTraffic="false"` and a `networkSecurityConfig` — the app has
some network capability (the Windows config mentions an update server
`fileDownPath=JCWXLCA09B/JCWXLCA09B_3.0.3`, `fileUpdatePath=Version`,
`AutoExamineUpdate=1`). **[V]** Not relevant to the port.
