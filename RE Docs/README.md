# Reverse Engineering Documentation — iScout / Mechanic-Ti Thermal Camera

**Target:** DYT (德奕通) `com.dyt.wcc` thermal-camera platform, shipped under many OEM brands
(Mechanic-Ti, iScout, Xtherm, Aixun, MaAnt, MileSeey, QianLi, …).
**Goal:** Port the vendor's Android/Windows application stack to Linux.

**Status of this document set:** produced from a single reconnaissance session.
Every factual claim is tagged:

| Tag | Meaning |
|-----|---------|
| **[V]** | **Verified** — reproduced from the binary/artifacts in this session. Evidence path or command is given. |
| **[I]** | **Inferred** — consistent with the evidence but not directly proven. Treat as a hypothesis to confirm. |
| **[?]** | **Unknown** — not established. Listed in `09-open-questions-and-next-steps.md`. |

Do **not** treat **[I]** or **[?]** items as established fact. This repository has previously been
polluted by fabricated reverse-engineering write-ups; the tagging exists to prevent a repeat.

---

## 1. What the device is

A USB "dual-vision" phone thermal camera module. **[V]** (from the bundled user manual
`assets/TYF0ReadMeEN.pdf` and `iScoutRead.pdf`)

| Property | Value |
|----------|-------|
| Thermal resolution | 256 × 192 |
| Spectral band | 8–14 µm |
| Lens | 3.2 mm |
| Measurement range | −15 °C … 600 °C |
| Visible camera | yes (second UVC stream) — "dual vision" |
| Interface | USB (UVC class + vendor control transfers) |
| Host support | Android (APK), Windows (Win32/.NET) |

Two firmware/product families appear in the artifacts: **TYF0 / TYF0C** (manual PDFs) and the
application codenames **CA09B, CA09D, CA30D, CAAnalyzer, CA_TA_10**.

## 2. Artifacts analysed

| Artifact | Size | Location |
|----------|------|----------|
| `MechanicTi.apk` | 169 MB | `../iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/` |
| `iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe` | 294 MB | same |
| Extraction workspace | — | `../RE Workspace/` (see `02-toolchain-and-reproduction.md`) |

## 3. Executive summary of findings

**The Linux port is very feasible.** The critical path is short and well understood:

1. **The camera is a UVC device driven by a *modified* libuvc.** **[V]**
   The APK ships `libuvc.so` whose embedded source paths are
   `jni/libuvc/android/jni/../../src/{init,device,ctrl,stream,frame,diag}.c` — i.e. upstream libuvc
   plus vendor additions: `uvc_diy_communicate`, `uvc_diy_start_preview`, `uvc_diy_stop_preview`,
   `uvc_find_device2`, `uvc_allocate_ini_frame`, `uvc_allocate_ini_preview_frame`.
   Upstream libuvc already runs on Linux → most of the transport is reusable as-is.

2. **The vendor command protocol is a plain control-transfer transaction.** **[V]**
   Write 8-byte command → poll 1-byte status → read 15-byte result. The **complete opcode table
   was recovered** from static `.rodata` constants in `libUVCCamera.so`, including the stream-start
   and raw-output commands, and the read/write paths are classified by risk. See
   `04-usb-protocol.md` §4.2 and §4.8.

3. **The thermometry math is fully recovered, with exact constants.** **[V]**
   `libthermometry.so` is only 13 KB and decompiles cleanly. It is a standard radiometric model
   (ε, τ, reflected + atmospheric radiance, Stefan–Boltzmann quartic inversion). See
   `05-thermometry-algorithm.md`.

4. **The Windows build exposes the same subsystem as named x86 DLLs** **[V]** —
   `libiruvc.dll`, `libircmd.dll`, `libirtemp.dll`, `Temperature.dll`, `Dcore.dll`,
   `UVCController.dll` — plus calibration tables that the Android build does not ship
   (`tau_H.bin`, `tau_L.bin`, `MILI6_*.bin`, `block_lut.dat`, 27 LUT palettes).
   All of these are now extracted, see `07-windows-app-and-installer.md`.
5. **Palette format is trivial.** **[V]** 768 bytes = 256 × RGB24, R first, index 0 = coldest.
   Identical in both builds.

6. **The Windows build leaks its source tree.** **[V]** Every native DLL carries an RSDS debug
   record with its original build path (`E:\SDK\libirtemp\Release\Win32\dll\`,
   `D:\WORK\ASIC_384_640_1280\mini384_640\…`, …), which reveals the SDK's structure and confirms
   it is shared across several sensor families. See `07-windows-app-and-installer.md` §3.2.

7. **The DYT image container is fully specified.** **[V]** A JPEG with the thermal payload spliced
   in as a run of `FF E2` (APP2) segments immediately after the APP0/APP1 headers, carrying a
   self-describing header blob followed by the raw data chunked at 0xFFFD bytes. See
   `06-asset-and-file-formats.md` §2.

8. **`configs_maintenanceguy.txt` is a device serial-number allow-list, not branding.** **[V]**
   Native code AES-decrypts each `;`-separated entry and compares the first 8 bytes against the
   camera's own serial. **[I]** The key is `"dyt1101c"` NUL-padded to 16 bytes, AES-128-CBC, which
   yields serials matching the vendor's `DYT`-prefixed format. See
   `06-asset-and-file-formats.md` §3.2-3.3.

9. **The thermal frame layout is fully solved — statically, without hardware.** **[V]**
   The frame is a flat, row-major `uint16` array of `width × (active_height + 4)`, `stride ==
   width`. The **last 4 rows are a reference/shutter band**, not image data, and they carry a
   per-unit calibration record (including a 32-byte serial key blob and the encrypted serial).
   The output is a `float` array of `width*(height-4) + 10`. Three independent code paths agree,
   and the record offsets were cross-checked at two different sensor widths. This was the last
   hard blocker. See `04-usb-protocol.md` §4.5.

10. **The USB VID/PID selects the device mode, and the mode selects the thermometry path.** **[V]**
    `UVCCamera::connect` maps `1514:0001` → mode `0x44C` (full radiometric, 4 reference rows),
    `0BDA:5840`/`0BDA:5830` → mode `1000` (trivial `raw/64 − 273.15`, no reference rows), and
    `0BDA:5846`/`0BDA:31DA`/`0581:0B00` → mode `0x3EB`. A port must reproduce this dispatch. See
    `04-usb-protocol.md` §4.10.

11. **The auto-shutter (flat-field) trigger is recovered and must be replicated.** **[V]**
    The host watches the reference band's second sample and issues `uvc_set_zoom_abs(cam,
    0xffff8000)` when it drifts by ≥ 15 counts. Omitting this produces drift and banding within
    minutes. See `04-usb-protocol.md` §4.5.5.

12. **Frame delivery is gated on a serial-number handshake for radiometric modes.** **[V]**
    If the device serial does not verify against the allow-list, `UVCPreviewIR+0xaf1` stays clear
    and the preview is suppressed. A port that skips the handshake will appear to receive nothing
    from a `1514:0001` unit. See `04-usb-protocol.md` §4.10.

13. **The thermometry port is written and byte-verified against the vendor library.** **[V]**
    `linux-port/tools/thermometry_diff/port.c` reproduces the vendor `libthermometry.so`'s LUT
    and output image **byte-for-byte** for **all four sensor widths** (240 / 256 / 384 / 640),
    in both `fix_mode` configurations (GetFix off and on), including NaN edge cases. `diff.py` is
    a CI-usable PASS/FAIL gate (exit 0 on PASS). The verification was done statically — no
    hardware. The thermometry layer is complete; the remaining port work is the `frame.c` /
   `control.c` layer that feeds live UVC frames into the verified port. See
   `08-linux-port-plan.md` §8.6.1.

14. **The Windows calibration tables are decoded, with byte-level proof of identity.** **[V]**
    `tau_H.bin`/`tau_L.bin` are 7168 bytes = `uint16[56][64]`; `MILI6_{H,L}.bin` are 7424 bytes =
    a 256-byte header plus the same table. Values are **atmospheric transmittance in Q14**
    (16384 = 1.0), indexed by target temperature (248.15 K … 1623.15 K, 25 K steps) and distance
    (0.25 m … 50 m), bilinearly interpolated. The readers' index arithmetic demands exactly those
    sizes, and the only blocks in the whole installer with those shapes are blk162/blk163 and
    blk134/blk135 — so identity is established by arithmetic, not by the (undecodable) NSIS
    filename table. See `10-calibration-tables.md`.

15. **The tables are Windows-only — Android computes τ analytically.** **[V]**
    A negative result with real consequences. The APK ships no calibration table of any kind, and
    `libthermometry.so` (13,856 B total) is too small to contain the 7,168 B table. `CalcFixRaw`
    evaluates τ in closed form (a two-band exponential fit, `τ_eff = 1.9·e1 − 0.9·e2`), while
    `libirtemp.dll` contains none of those coefficients and is table-only. **The two vendor builds
    use structurally different transmittance models for the same hardware**, so the decoded tables
    are *not* on the port's critical path: `calib.c` is complete and tested but deliberately
    unwired, and the pipeline keeps the byte-verified analytic path. Which model matches this unit
    needs a two-distance hardware comparison. See `10-calibration-tables.md` §4.5.

## 4. Document map

| File | Contents |
|------|----------|
| `01-target-and-hardware.md` | Product families, USB VID/PIDs, device identification |
| `02-toolchain-and-reproduction.md` | Exact commands to rebuild every artifact in this analysis |
| `03-android-app-architecture.md` | APK layout, Java packages, native library graph, command dispatch |
| `04-usb-protocol.md` | **Core porting reference** — UVC setup + vendor command protocol |
| `05-thermometry-algorithm.md` | **Core porting reference** — full math + constants |
| `06-asset-and-file-formats.md` | Palettes, calibration tables, config files, DYT image container |
| `07-windows-app-and-installer.md` | Bootstrapper analysis, NSIS extraction, full file inventory |
| `08-linux-port-plan.md` | Proposed architecture and phased plan for the Linux port |
| `09-open-questions-and-next-steps.md` | Prioritised list of what is still unproven |
| `10-calibration-tables.md` | **Core porting reference** — `tau_*.bin` / `MILI6_*.bin` formats |

## 5. Single most important next action

**The frame parser and LUT pipeline are written and byte-verified — no hardware needed.** **[V]**

The portable C port at `linux-port/tools/thermometry_diff/port.c` reproduces the vendor
`libthermometry.so`'s `thermometryT4Line` (16,384-entry LUT) and `thermometrySearch` (10-float
header + `width×(height-4)` pixel temperatures) **byte-for-byte** for **all four sensor widths**
(240 / 256 / 384 / 640), in both `fix_mode` configurations, including NaN edge cases. `diff.py`
is a CI-usable gate (exit 0 on PASS). The thermometry layer is complete. See
`08-linux-port-plan.md` §8.6.1 for the verified SHA256 hashes and reproduce commands.

The next no-hardware step:
1. **Write the `frame.c` / `control.c` skeletons** against `04-usb-protocol.md` §4.5/§4.6 so the
   verified port becomes an end-to-end `raw frame → float temperatures` module ready for a live
   UVC stream. The `uvc_diy_communicate` control primitive and the frame layout are both fully
   specified statically.

When the device arrives, the first command is `lsusb -v` (identifies the VID/PID → mode, and the
`bFormatIndex` of the thermal stream). Everything else needed for the port is already known.

The Windows calibration tables (`tau_*.bin`, `MILI6_*.bin`) were the last substantive unknown and
are now decoded — see `10-calibration-tables.md` and `linux-port/src/calib.c`. Decoding them also
produced a **negative result that narrows the port**: the Android build ships no table and computes
τ analytically instead, so the tables are not on the critical path (§4.5 of that document).
`block_lut.dat` remains undecoded but is not read by any native thermometry code, so it does not
block the port.

Two calibration questions stay open, both needing hardware: which of `tau_H`/`tau_L` and
`MILI6_H`/`MILI6_L` is which (cosmetic, runtime-overridable), and **whether the table model or the
analytic model better matches this unit**.

The second one turned out to need a better experiment than originally planned. Measuring the three
models side by side without hardware (`linux-port/tools/taucmp.c`) showed that **`tau_*.bin` is flat
from 3.00 m to 50 m** — 48 of its 56 rows are bit-identical over that whole span — and that it is
periodic with period 14 rows (4 byte-identical humidity planes). `MILI6_*.bin` has neither problem.
So `tau_*.bin` is unusable beyond ~3 m, and the planned "compare at 0.5 m and 3 m" probe would have
landed exactly on the degenerate knee.

A dry run of the corrected experiment then pinned down the source temperature. The discriminator is
the apparent-temperature drop from 2 m to 10 m, and against a 0.2 K repeatability floor it separates
the models by only **1.2× at 50 °C and 2.2× at 100 °C** — too marginal to conclude — but **41.7× at
300 °C**. The experiment needs a **300 °C** blackbody, sampled at **1 m, 2 m and 10 m**. See
`10-calibration-tables.md` §3.4, §8.3 and §8.4.

## 6. Safety / legal note

These artifacts are the vendor's proprietary software. This analysis was performed on
locally-owned hardware and software for the purpose of interoperability and running the
hardware on a platform the vendor does not support. No protection measure was circumvented to
obtain the firmware; all analysis was static, on files provided with the product.
