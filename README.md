# Mechanic iScout Thermal Camera — Linux Port

Porting the Mechanic iScout / DYT (`com.dyt.wcc`) USB thermal camera to Linux
(`Mechanic-Iscout-Thermal-Camera-linux-Port`). The vendor ships Android and Windows
software only; there is no official Linux support.

The unit is a UVC "dual-vision" module — 256×192 radiometric thermal plus a visible
camera, 8–14 µm, −15 °C … 600 °C — sold under OEM brands including Mechanic iScout,
Mechanic-Ti, Xtherm, Aixun, MaAnt, MileSeey, QianLi, etc.

**Status (v0.1.0):** the eight-phase engine roadmap (`RE Docs/08` §8) is complete —
units and display, measurement and alarms, device read-back and the one parameter
write, the visible half and fusion, stills and the DYT container, mp4 recording, and
the 2× super-resolution zoom. The frame→temperature pipeline is byte-verified against
the vendor's own `libthermometry.so` for all four sensor widths in both `fix_mode`
configurations, live capture and recording work on hardware, and the super-resolution
output matches the vendor's own model to within one LSB.

**Not yet established:** absolute temperature accuracy — nothing has been compared
against a calibrated reference, so the atmospheric-transmittance model is still `[?]`
(`RE Docs/09` §9). The visible and thermal planes are 1:1 in grid terms by construction,
and `0,0` is the vendor's own alignment default, but the **optical boresight** of the two
lenses has not been measured (`RE Docs/04` §4.10). The remaining `[?]` items are listed
in `RE Docs/09`.

## Layout

| Path | What |
|---|---|
| `RE Docs/` | The reverse-engineering write-up. **Start here.** |
| `linux-port/` | The port: library, tests, CLI tools, vendored libuvc. |
| `RE Workspace/` | Generated analysis material (extracted APK, decompilation, NSIS blocks). Disposable; the scripts that regenerate it are tracked. |
| `iScout … v3.0.6+windows/` | The vendor originals. Not tracked. |

## Build and test

```bash
git clone https://github.com/akbar-npj/Mechanic-Iscout-Thermal-Camera-linux-Port.git
cd Mechanic-Iscout-Thermal-Camera-linux-Port/linux-port
make            # build
make check      # regression gate — exits non-zero on any failure
make clean
```

`make check` runs the unit-test binaries, the `probe --selftest` safety check, and
byte-compares the frame pipeline against frozen vendor ground truth for all four sensor
widths in both `fix_mode` configurations. It needs no camera attached.

`cc`, `make` and `ar` are all that is required. `libusb-1.0`, OpenCV, Qt6 and MNN are
each **optional** and detected at build time — each one only adds a tool or a feature,
and the engine and its tests build without any of them.

**See [`BUILDING.md`](BUILDING.md)** for the per-distribution package lists (Fedora and
Debian/Ubuntu), the install targets, the udev rule that gives the app access to the
camera, and packaging. The JPEG backend is the vendored stb, so it is always present;
`make JPEG=libjpeg` selects libjpeg as an accelerator instead. No network access is
needed — libuvc and stb are vendored and built from source.

## Where to start reading

The docs are tagged **[V]** verified / **[I]** inferred / **[?]** unknown. The tagging is
deliberate and load-bearing: the project has previously contained fabricated write-ups, so
do not treat **[I]** or **[?]** as established fact.

| | |
|---|---|
| `RE Docs/README.md` | Orientation and the executive summary of findings |
| `RE Docs/04-usb-protocol.md` | The vendor control-transfer protocol and opcode table |
| `RE Docs/05-thermometry-algorithm.md` | The radiometric model |
| `RE Docs/08-linux-port-plan.md` | The port plan, as built |
| `RE Docs/09-open-questions-and-next-steps.md` | What is still unproven |
| `RE Docs/10-calibration-tables.md` | The Windows calibration tables |

`RE Docs/02-toolchain-and-reproduction.md` is the recipe for regenerating `RE Workspace/`
from the two vendor originals; `RE Docs/07` and `RE Docs/10` cover the Windows DLLs and
the calibration tables in more detail.

## Safety

**Never send a WRITE-classified opcode to the device.** The write path can destroy the
factory calibration, and that cannot be reconstructed from anything in this repository —
the Windows calibration tables are not confirmed to hold the same data.

`RE Docs/04-usb-protocol.md` §4.8 classifies every recovered function as reads-only,
stream control, or dangerous-write. `probe` implements only the first two: it refuses any
opcode whose table entry is marked `WRITE` (`linux-port/tools/probe.c:279`), and
`probe --selftest` exercises that guard without a device attached.

## Not tracked

The vendor installer (294 MB) and APK (169 MB), everything derived from them, all build
output, and the reference material kept beside the repo for cross-checking
(`Thermal-Camera-Redux/`, and `MechaniscoutPcap/` — 1.1 GB of USB captures). This
repository tracks *work only* — 9.6 MB across 245 files. See `.gitignore`, which documents
each rule.

## License

GPL-3.0 — see `LICENSE`.

The vendored components in `linux-port/third_party/` keep their own terms: `libuvc` is BSD
(`LICENSE.txt`) and `stb` is dual MIT / public domain (`LICENSE`). MNN is not vendored — the
recipe in `linux-port/third_party/README.md` fetches and builds it — and is Apache-2.0.

The vendor's own binaries, and everything derived from them, are not distributed here; see
`.gitignore`.
