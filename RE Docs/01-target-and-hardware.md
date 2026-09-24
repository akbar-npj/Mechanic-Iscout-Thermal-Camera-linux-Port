# 01 — Target, Hardware and Device Identification

## 1.1 Product identity

The application is a **rebadged OEM platform**. One codebase ships to many vendors; the brand is
selected at runtime by a `COMPANY_*` constant and per-brand asset/config files.

**[V]** `com/dyt/wcc/constans/DYConstants.java` defines the OEM list:

```
COMPANY_DYT, COMPANY_AiXun, COMPANY_DOBIY_CN, COMPANY_DOBIY_EN, COMPANY_HENXTECH,
COMPANY_HT820, COMPANY_JMS, COMPANY_KEPURUIMT, COMPANY_MILESEEY, COMPANY_MSL256I,
COMPANY_MTI448, COMPANY_MaAnt, COMPANY_MarMonix, COMPANY_NEUTRAL, COMPANY_PERGAM,
COMPANY_QIANLI, COMPANY_RADIFEEL, COMPANY_RUOSHUI, COMPANY_TESLONG, COMPANY_TLETEK,
COMPANY_VICTOR, COMPANY_VOTIN, COMPANY_YouLiDe, COMPANY_ZhiXin, COMPANY_ACEGMET_TI256,
COMPANY_MaintenanceGuy
```

Per-brand customisation lives in `com/dyt/wcc/customize/<brand>/`
(`aixun`, `dobiyen`, `dobiycn`, `dyt`, `ht820`, `jms`, `kepuruimt`, `maant`, `marmonix`,
`maintenanceguy`, `msl256i`, `mti448`, `neutral`, `tletek`, …). **[V]**

The analysed build identifies as:

| Field | Value | Evidence |
|-------|-------|----------|
| Android package | `com.dyt.wcc.maintenanceguy` | `AndroidManifest.xml` **[V]** |
| versionName | `3.04.07` (versionCode 571) | `AndroidManifest.xml` **[V]** |
| minSdk / targetSdk | 26 / 34 | `AndroidManifest.xml` **[V]** |
| Windows product | `ThermalAnalysisSystem.exe`, version `3.0.6` | NSIS header string table **[V]** |
| Windows vendor strings | `www.mechanichk.com`, `www.mechanic.hk`, `Mechanic Creation` | NSIS header **[V]** |
| Windows install key | `…\Uninstall\Mechanic-TiVisualPlatform` | NSIS header **[V]** |

> Note the version numbers differ between platforms (Android `3.04.07` vs Windows `3.0.6`).
> They are independent versioning schemes for the same product generation. **[I]**

## 1.2 Sensor / optical specification

**[V]** from `assets/TYF0ReadMeEN.pdf` (18 pp) and `assets/iScoutRead.pdf` (44 pp):

| Property | Value |
|----------|-------|
| Thermal resolution | 256 × 192 |
| Working wavelength | 8 – 14 µm |
| Lens | 3.2 mm |
| Measurement range | −15 °C … 600 °C (5 °F … 1112 °F) |
| Frame rate | not stated in the manual **[?]** |
| Visible channel | present ("dual vision") |

Firmware/product family strings found in the artifacts: **TYF0**, **TYF0C** (manuals),
**MILI6** (calibration table names, see §1.4). **[V]**

## 1.3 USB device identification

**[V]** The APK ships two identical USB device filters:

`resources/res/xml/device_filter.xml` and `resources/res/xml/dy_device_filter.xml`

```xml
<usb>
    <usb-device product-id="1"     vendor-id="5396"/>   <!-- 0x1514:0x0001 -->
    <usb-device product-id="22592" vendor-id="3034"/>   <!-- 0x0BDA:0x5840 -->
    <usb-device product-id="22576" vendor-id="3034"/>   <!-- 0x0BDA:0x5830 -->
    <usb-device product-id="22598" vendor-id="12762"/>  <!-- 0x31DA:0x5846 -->
    <usb-device product-id="2816"  vendor-id="1409"/>   <!-- 0x0581:0x0B00 -->
</usb>
```

Converted to the conventional hex form:

| VID:PID | Notes |
|---------|-------|
| `1514:0001` | vendor-specific VID `0x1514` |
| `0BDA:5840` | `0x0BDA` = **Realtek**. Likely the UVC bridge / ISP in the module. **[I]** |
| `0BDA:5830` | Realtek, sibling of the above **[I]** |
| `31DA:5846` | vendor-specific |
| `0581:0B00` | vendor-specific |

**These IDs are no longer just a filter list — they select the device mode.** **[V]**
`UVCCamera::connect(int vid, int pid, …)` (`libUVCCamera.so @ 0x15c28c`) branches on the pair and
stores a mode that governs the entire thermometry path (see `04-usb-protocol.md` §4.10):

| VID:PID | Mode | Thermometry | Notes |
|---------|------|-------------|-------|
| `1514:0001` | **`0x44C` (1100)** | full radiometric, 4 reference rows, `w×(h−4)` output | 256×192 class |
| `0BDA:5840` | `1000` | direct `raw/64 − 273.15`, `w×h` output | `0x0BDA` = **Realtek** |
| `0BDA:5830` | `1000` | same | Realtek |
| `0BDA:5846` | `0x3EB` (1003) | third variant | Realtek |
| `0BDA:31DA` | `0x3EB` (1003) | third variant | transpose of the XML entry |
| `0581:0B00` | `0x3EB` (1003) | third variant | |
| *(other)* | `0` | unsupported — the app refuses the device | |

> **Discrepancy:** the APK's `device_filter.xml` lists `31DA:5846`, but `connect` tests
> `VID 0x5846 / PID 0x31DA` — the transpose. Four of five entries match exactly; this one does
> not. **[V]**

**The 256×192 unit in §1.2 is almost certainly `1514:0001` → mode `0x44C`** — that is the
radiometric path, and it is the only one with a dedicated 256-wide calibration table in
`thermometryT4Line` and a `256 × 192 × 2` reference-band offset in `do_preview`. **[I]**

> **Actionable on arrival:** `lsusb` identifies which row applies. If it is `1514:0001`, the port
> must also complete the serial-number handshake before the device will emit thermal frames —
> see `04-usb-protocol.md` §4.10. If it is a `0BDA:*` part, frames flow immediately and the
> thermometry is the trivial `raw/64 − 273.15` form.

> **Correction (2026-09-24, live device).** The unit in hand is **`0bda:5840`** — Realtek
> "USB Camera", manufacturer `Generic`, serial `200901010001` — i.e. mode **`1000`**, so the
> `1514:0001`/`0x44C` inference above was **wrong** (see `04-usb-protocol.md` §4.10). Two further
> corrections:
>
> * **"frames flow immediately" is wrong for mode `1000`.** The device streams a flat `0x8000`
>   placeholder (`238.85 C`) until the host sends `setTinyCOutputADValue` *after* the UVC stream is
>   running; real data follows ~2–3 s later. Sending that order before streaming latches the device
>   at status `0x0e` until a replug.
> * **No serial handshake is needed here.** `isVerifySN` is gated on the radiometric modes only, so
>   a mode-`1000` device emits frames without it.
>
> Confirmed live: 256×192, `bFormatIndex 1`, 25 fps, and `raw/64 − 273.15` reproduces the vendor's
> temperatures exactly.

The Android manifest also declares `<uses-feature android:name="android.hardware.usb.host"
android:required="true"/>`. **[V]**

## 1.4 Branding / calibration identifiers found in the Windows build

**[V]** NSIS header string table (see `07-windows-app-and-installer.md`):

- Application variants: `CA09B.exe`, `CA09D.exe`, `CA30D.exe`, `CAAnalyzer.exe`,
  `ThermalAnalysisSystem.exe`
- Calibration / data files: `block_lut.dat`, `tau_H.bin`, `tau_L.bin`,
  `MILI6_H_500.bin`, `MILI6_H_91.bin`, `MILI6_L_500.bin`, `MILI6_L_91.bin`
- 27 palettes: `Luts/lut_1.dat` … `Luts/lut_27.dat`
- Native DLLs: `libiruvc.dll`, `libircmd.dll`, `libirtemp.dll`, `libirparse.dll`,
  `libirprocess.dll`, `Temperature.dll`, `Dcore.dll`, `DcontrolDll.dll`, `Dimgproc.dll`,
  `UVCController.dll`, `Tiny1BDll.dll`, `Tiny1CDll.dll`, `XthermDll.dll`, `jpegext.dll`

The `MILI6_*_91` / `MILI6_*_500` naming suggests two calibration variants (91 and 500 — possibly
lower/upper temperature range endpoints, or two sensor grades). **[I]** Not confirmed.

The Android build ships only a reduced asset set (`assets/1.dat` … `assets/7.dat`), so **the
Windows installer is the better source for calibration data.** **[V]**

## 1.5 Host-side requirements observed

**[V]** From the NSIS header install script:

- The Windows app is .NET (installs .NET Framework 4.6.1 via `NDP461-KB3102436-x86-x64-AllOS-ENU.exe`).
- Ships VC++ redistributables for 2008, 2010, 2012, 2013 and 2015 (x86 + x64).
- Uses OpenCV (`cvextern.dll`, `OpenCvSharp.dll`, `opencv_ffmpeg310_64.dll`),
  FFmpeg (`avcodec`/`avformat`/`avutil`/`avfilter`/`avdevice`/`swscale`/`swresample`/`postproc`),
  AForge.NET, Accord.NET, SharpGL, ZedGraph.
- Executes `M1.exe` and `M2.exe` during install (purpose unknown). **[?]**

This is a heavy Windows-only GUI stack; **none of it needs to be reproduced on Linux** — it is
presentation layer only. The reusable parts are the transport (`libiruvc`) and the math
(`libirtemp` / `Temperature.dll`), which are mirrored by the Android native libraries.
