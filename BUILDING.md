# Building, installing and running

This is the build guide for the Linux port. Everything happens inside
`linux-port/`; the reverse-engineering write-up lives in `RE Docs/` and is not
needed to build.

- [What gets built](#what-gets-built)
- [Dependencies](#dependencies)
- [Build](#build)
- [Test](#test)
- [Run from the build tree](#run-from-the-build-tree)
- [Install](#install)
- [Giving the app access to the camera](#giving-the-app-access-to-the-camera)
- [Packages](#packages)
- [Troubleshooting](#troubleshooting)

## What gets built

| Target | What it is | Built when |
|---|---|---|
| `build/libdyt.a` | the engine: thermometry, frame pipeline, display, measurement, alarms, palettes, JPEG/DYT stills, the super-resolution seam | always |
| the unit-test binaries | the regression suite `make check` runs | always |
| `build/probe`, `build/capture_demo` | device interrogation and capture demos (read-only) | `libusb-1.0` present |
| `build/dytview` | the OpenCV live viewer | `libusb-1.0` **and** OpenCV |
| `build/dytrec`, `build/player_test` | the mp4 recorder, and the reader's round-trip gate | OpenCV |
| `build/dytqt` | the Qt6 desktop application | Qt6 Widgets |

Every dependency is **detected, never required**. A host with only a C compiler
builds the engine and passes `make check`; each optional piece simply removes a
tool. Nothing here needs network access — libuvc and stb are vendored and built
from source.

## Dependencies

### Fedora

```sh
sudo dnf install gcc gcc-c++ make pkgconf-pkg-config \
    libusb1-devel qt6-qtbase-devel opencv-devel ImageMagick
```

To build the RPM as well: `sudo dnf install rpm-build rpmdevtools`.

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential pkg-config \
    libusb-1.0-0-dev qt6-base-dev libopencv-dev imagemagick
```

To build the .deb as well: `sudo apt install dpkg-dev` (it provides
`dpkg-shlibdeps`; `dpkg-deb` itself is part of the essential `dpkg`).

Qt6 needs Debian 12 (bookworm) or Ubuntu 22.04 or newer. On an older release
the Qt6 packages do not exist — drop `qt6-base-dev` and everything except the
GUI still builds.

`ImageMagick` is only needed by `make install`, which rasterises the icon. The
build and `make check` do not use it.

### What each dependency buys you

| | with it | without it |
|---|---|---|
| `cc`, `make`, `ar` | everything | nothing builds |
| `libusb-1.0` | the live-capture tools `capture_demo` and `probe`, and the vendored libuvc they build on | the byte-verified pipeline and its unit tests still build and `make check` still passes; only the live-capture tools are omitted |
| OpenCV | the live viewer `dytview` and the mp4 recorder/reader (`dytrec`, `player_test`, and clip playback in the GUI) | those tools are omitted, and the GUI's gallery says clips need OpenCV |
| Qt6 Widgets | the desktop app `dytqt` | only the CLI tools are built |
| Qt6 OpenGL Widgets | the 3D Analysis view's GL renderer | the view falls back to software rendering; everything still works |
| MNN | the 2× super-resolution zoom | `dyt_mnn_zoom2()` reports unavailable and refuses rather than inventing a frame |
| libjpeg | an accelerator for the JPEG codec | the vendored stb is used instead |

Qt6 OpenGL Widgets is part of the Qt6 base module, so it normally needs
nothing beyond `qt6-qtbase-devel` / `qt6-base-dev`. Confirm with
`pkg-config --exists Qt6OpenGLWidgets`; if it is missing, the build simply
compiles the view without its GL renderer.

The JPEG backend is the vendored stb by default, because it is always present
and produces identical bytes on every machine. libjpeg is opt-in:

```sh
make JPEG=libjpeg
```

### Super-resolution (MNN) — optional

MNN is not vendored. `linux-port/third_party/README.md` has the full recipe
(fetch, one required patch, CMake build, install prefix). Once an install
exists, point the build at it:

```sh
make MNN_ROOT=third_party/mnn-install
```

`MNN_ROOT` defaults to `third_party/mnn-install`, and the runtime is enabled
only when that directory carries `include/MNN/Interpreter.hpp` **and**
`lib/libMNN.so`. The CMake build needs `cmake` and `ninja-build` on either
distribution.

## Build

```sh
cd linux-port
make
```

`make -j"$(nproc)"` works. One flag is **mandatory** and lives in the Makefile:
`-ffp-contract=off`. The thermometry reproduces the vendor's
`libthermometry.so` byte-for-byte only with FP contraction disabled and every
fused multiply-add written explicitly; overriding `CFLAGS` without keeping it
will make the frozen-ground-truth diff fail. See `RE Docs/08` §8.6.1.

## Test

```sh
make check
```

This runs the unit-test binaries, the `probe --selftest` safety check, the
packaging cross-checks, and byte-compares the frame pipeline against frozen
vendor ground truth for all four sensor widths in both `fix_mode`
configurations. It needs **no camera attached**, and exits non-zero on any
failure — it is the CI gate.

A useful variant is a build with no optional runtime at all, which exercises
the declining stubs:

```sh
make MNN_ROOT=/nonexistent check
```

## Run from the build tree

```sh
./build/dytqt
```

The app finds its palettes and model relative to the current directory and to
its own location, so a source-tree run uses `linux-port/palettes/` and
`linux-port/models/` without an install step. With no camera attached it opens
in its `NO SIGNAL` state.

The CLI tools:

```sh
./build/probe          # list the camera and its capabilities (read-only)
./build/dytview        # OpenCV viewer (needs OpenCV)
./build/dytqt --selftest
```

## Install

```sh
sudo make install               # PREFIX=/usr/local (default)
sudo make install PREFIX=/usr   # /usr
```

The default is `/usr/local`. What is installed under `$(PREFIX)`:

| Path | What |
|---|---|
| `bin/dytqt` | the application |
| `share/applications/dytqt.desktop` | the menu entry |
| `lib/udev/rules.d/Mechanic-iScout-Thermal-Camera.rules` | the camera-access rule (below) |
| `share/dytqt/palettes/*.dat` | the 28 vendor palettes |
| `share/dytqt/models/zoom2.mnn` | the super-resolution model |
| `lib/dytqt/libMNN.so` | the MNN runtime, when the build linked one |
| `share/icons/hicolor/<size>/apps/dytqt.png` | the icon, at 7 sizes |

`sudo make uninstall` removes exactly those files. Both `make install` and
`make uninstall` honour `DESTDIR` for staged installs.

`make install` deliberately does **not** touch running system state: it copies
the udev rule into place but does not reload udev (a developer's own install
should not change a live system). The packages' post-install scripts do the
reload — so after a bare `make install` you must do it yourself; see the next
section.

## Giving the app access to the camera

The camera is a plain USB device, and by default only root may open it. The
repository ships a udev rule that grants access to everyone, and both packages
install it.

### The rule

`linux-port/packaging/Mechanic-iScout-Thermal-Camera.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="1514", ATTR{idProduct}=="0001", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{idProduct}=="5840", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{idProduct}=="5830", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{idProduct}=="5846", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0bda", ATTR{idProduct}=="31da", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="0581", ATTR{idProduct}=="0b00", MODE="0666"
```

Those six VID:PID pairs are the whole DYT/Mechanic-Ti family — the same list
`tools/probe.c` probes for, and `make check` fails if the two ever drift.

`MODE="0666"` makes the device world-writable, which is what a single-user
workstation wants. On a shared machine, the stricter alternative is
`TAG+="uaccess"`, which grants access only to the logged-in seat's user — but
it needs systemd-logind to honour the tag, so it does not work in a container
or over a plain serial console. The explicit mode is used here for that reason;
the file's header comment records the trade-off.

### Installing it

`make install` puts the rule under `$(PREFIX)/lib/udev/rules.d/`. Both
`/usr/lib/udev/rules.d` and `/usr/local/lib/udev/rules.d` are in udev's search
path, so either prefix works.

udev does not notice a new file in that directory on its own. After a bare
`make install`, reload it:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
```

The .deb and .rpm do this for you from their post-install script. After a
reload, **unplug and replug the camera** (or run the `trigger` above) so the
new permissions are applied to the device node that already exists.

### If you already have a rule

A rule with a **different name** in `/etc/udev/rules.d/` coexists with the
packaged one — both are applied, and duplicate `MODE=` settings are harmless.
So an existing `/etc/udev/rules.d/99-dyt-thermal.rules` (or similar) is fine to
leave in place; nothing needs to be removed.

A rule with the **same name** in `/etc/udev/rules.d/` shadows the packaged file
completely. That is the intended override for an administrator who wants
different permissions: put a file of the same name in `/etc` and the packaged
one is ignored. Upgrading the package replaces `/usr/lib/udev/rules.d/…` in
place and never touches `/etc`.

### Verifying

With the camera plugged in:

```sh
lsusb | grep -i -e 1514 -e 0bda -e 0581     # the device is present
./build/probe                                # the app can open it
ls -l /dev/bus/usb/00X/00Y                   # should show crw-rw-rw-
```

If `probe` reports a permission error, the rule is not in effect — re-run the
two `udevadm` commands and replug.

### No `uvcvideo` blacklist is needed

The camera also binds the kernel's `uvcvideo` driver, and older guides suggest
blacklisting it. That is **not** necessary here: the vendored libuvc detaches
the kernel driver itself when it claims the interface
(`uvc_claim_if`, `third_party/libuvc/src/device.c`) and re-attaches it on
close. Blacklisting `uvcvideo` would only remove the camera's V4L2 node, which
other software may want.

That V4L2 node (`/dev/videoN`, named "USB Camera") is worth knowing about: it
is the visible half as a normal webcam, so a program that grabs the first
video device can collide with the thermal app. `dytqt` uses libusb, not V4L2,
so it is unaffected — but a thermal-on-microscope overlay that expects the
thermal image on `/dev/videoN` will get the visible one.

## Packages

Both packages stage through `make install`, so they cannot drift from it.

```sh
make deb    # build/dytqt_<version>_<arch>.deb
make rpm    # build/dytqt-<version>-<release>.<arch>.rpm
```

`make deb` needs `dpkg-deb` (and uses `dpkg-shlibdeps` to measure the
dependencies when `dpkg-dev` is present, falling back to a default list
otherwise — the recipe prints which it used). `make rpm` needs `rpmbuild`.

`make rpm` takes a few variables, all with working defaults: `RPM_TOPDIR`,
`RPM_STAGE` (both must be space-free paths), and `PREFIX` is set to `/usr`.
Whether libMNN ships is decided by the same `HAVE_MNN` that decides whether
`make install` ships it, so the two cannot disagree.

## Troubleshooting

**`pkg-config: command not found`.** Install `pkgconf-pkg-config` (Fedora) or
`pkg-config` (Debian).

**The GUI is not built.** Qt6 Widgets was not found. Check
`pkg-config --exists Qt6Widgets`; if it fails, install `qt6-qtbase-devel` /
`qt6-base-dev`.

**Super-resolution is unavailable.** No MNN runtime was found. This is not an
error — see the MNN section above to build one, then rebuild.

**The app opens but shows `NO SIGNAL`.** Either no camera is attached, or the
udev rule is not in effect. See [Verifying](#verifying).

**`make install` fails on the icon.** ImageMagick is missing. Install
`ImageMagick` / `imagemagick`, or use the packages, which rasterise the icon
during staging.

**`make check` fails on the frame pipeline.** The build is not using
`-ffp-contract=off`; something overrode `CFLAGS`. Restore it — see
[Build](#build).
