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
* `libMNN*` is Alibaba MNN (neural inference). Its role in this app is **not established**
  **[?]** — possibly super-resolution. It is *not* on the critical path for basic capture.

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
