# Thermal Camera — Linux port

Porting a DYT (`com.dyt.wcc`) USB thermal camera to Linux. The vendor ships Android
and Windows software only; there is no Linux support.

The unit is a UVC "dual-vision" module — 256×192 radiometric thermal plus a visible
camera, 8–14 µm, −15 °C … 600 °C — sold under many OEM brands (Mechanic-Ti, iScout,
Xtherm, Aixun, MaAnt, MileSeey, QianLi, …).

**Status:** the frame→temperature pipeline is ported and byte-verified against the
vendor's own `libthermometry.so`, and the read-only bring-up path is complete.
Everything requiring a live device is still open — see `RE Docs/09`.

## Layout

| Path | What |
|---|---|
| `RE Docs/` | The reverse-engineering write-up. **Start here.** |
| `linux-port/` | The port: library, tests, CLI tools, vendored libuvc. |
| `RE Workspace/` | Generated analysis material (extracted APK, decompilation, NSIS blocks). Disposable; the scripts that regenerate it are tracked. |
| `iScout … v3.0.6+windows/` | The vendor originals. Not tracked. |

## Build and test

```bash
cd linux-port
make            # build
make check      # regression gate — exits non-zero on any failure
make clean
```

`make check` runs the four unit-test binaries, the `probe --selftest` safety check, and
byte-compares the frame pipeline against frozen vendor ground truth for all four sensor
widths in both `fix_mode` configurations.

Requires `cc`, `make`, and `ar`. `libusb-1.0` is **optional**: with it, the live-capture
tools `capture_demo` and `probe` (and the vendored libuvc they build on) are included;
without it, the byte-verified pipeline and its unit tests build and `make check` still
passes — only the live-capture tools are omitted. No network access is needed — libuvc is
vendored and built from source when libusb is present.

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

The vendor installer (294 MB) and APK (169 MB), everything derived from them, and all
build output. This repository tracks *work only* — roughly 7.5 MB. See `.gitignore`, which
documents each rule.
