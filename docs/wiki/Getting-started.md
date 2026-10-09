# Getting started

This page covers installing the app, opening it, and the parts of the window.

## Installing

On Fedora or a Debian/Ubuntu system, the simplest route is the package:

- **Fedora:** `sudo dnf install ./dytqt-*.rpm`
- **Debian/Ubuntu:** `sudo dpkg -i dytqt_*.deb`

Then launch **Mechanic iScout Thermal Camera** from your applications menu, or
run `dytqt` from a terminal.

If you are building from source instead, see [`BUILDING.md`](../../BUILDING.md).
The rest of this page assumes the app is installed and opens.

> **Why is the command called `dytqt`?** The product is *Mechanic iScout Thermal
> Camera*, but the program file and packages keep the short name `dytqt`. It is
> the same thing.

## Giving the app access to the camera

On Linux, USB devices are owned by `root` unless a rule says otherwise. The
packages install a small rule that lets your user open the camera, and reload it
for you. If you installed by hand, you must reload it yourself:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
```

Then **unplug and replug** the camera. The full detail, including what to do if
you already have your own rule, is in [`BUILDING.md`](../../BUILDING.md#giving-the-app-access-to-the-camera).

## Opening the app

Run `dytqt`, or launch it from the menu. Two things can happen:

- **A camera is attached** — after a moment the live heat picture appears.
- **No camera is attached** — the app replays a saved sample frame instead, so you
  can still explore the software. The bottom line of the window says `FIXTURE`
  rather than `LIVE`, so you always know which you are looking at.

The app also accepts options when run from a terminal (a colour palette, a zoom
level, a folder to save into, and so on). You do not need any of them for normal
use. `dytqt --help` lists them all.

## The window at a glance

The window has four regions:

```
┌────────┬──────────────────────────────────┬─────────────┐
│        │                                  │             │
│  Icon  │            The picture           │  Control    │
│  rail  │        (+ colour bar on right)   │  panel      │
│        │                                  │  (tabs)     │
│        ├──────────────────────────────────┤             │
│        │          Status strip            │             │
└────────┴──────────────────────────────────┴─────────────┘
```

### The icon rail (left)

A vertical strip of icons. Top to bottom:

| Icon | What it does |
|---|---|
| **Palette** | Opens the colour-palette chooser (how hot/cold is coloured) |
| **Fusion** | Opens the imaging-mode chooser (thermal only, visible only, blend, and so on) |
| **Mark** | Re-arms your last note-drawing tool; **right-click** offers Text or Arrow |
| **Rotate** | Turns the picture a quarter turn clockwise |
| **Compare** | Jumps to the Comparison tab |
| **Reset** | Undoes zoom, mirroring and rotation, and returns the range to automatic |
| **Tutorials** | Opens the in-app guide (the same one `F1` and `?` open) |
| **Contact** | Manufacturer contact information |
| **Setting** | Opens the Settings dialog |

Every icon does exactly what its keyboard key does — clicking is never a
different, subtly-behaving version of pressing a key.

### The picture (centre)

The thermal image, drawn as large as the window allows. To its right is a
**colour bar**: a vertical scale showing which colour means which temperature,
with the frame's hottest and coldest values at the ends. The bar's two handles
let you narrow or widen the range of temperatures shown — see
[The picture](The-picture.md#the-colour-bar-and-its-handles).

### The status strip (bottom)

Three lines of text that summarise what is going on:

| Line | Shows | Example |
|---|---|---|
| 1 | The current mode, imaging pattern, palette, unit, zoom and frame count | `mode 1000 \| fusion ir \| 01-iron-red.dat 1/28 \| C \| x2- \| 25 frames` |
| 2 | The current measuring tool, or a measurement's result | `tool: none (p point, l line, b box, n clear)` |
| 3 | A coloured dot, the range mode, the device state, the camera's serial and the frame rate | `● range auto \| LIVE Camera SN: CA09DDC00212 25.0 fps` |

**The dot on line 3 tells you the camera's health at a glance:**

| Dot | State word | Meaning |
|---|---|---|
| **Green** | `LIVE` | The camera is connected and streaming |
| **Amber** | `CONNECTING` / `WARMING UP` | Coming up, or briefly gone quiet |
| **Amber** | `NO SIGNAL` | The stream has frozen — the last picture is held on screen |
| **Red** | `NO DEVICE` | No camera could be found |
| **Grey** | `FIXTURE` | No camera is expected; a saved sample is being replayed |

A frame rate of `0.0 fps` is shown only in the states where it means something —
it is the honest evidence that a `NO SIGNAL` picture is frozen rather than live.

### The control panel (right)

A set of tabs holding the actual controls. Everything on the panel is a button
version of a keyboard key, so the two can never disagree.

| Tab | What is on it |
|---|---|
| **Troubleshoot** | The main working tab: circuit mode, temperature measurement, notes (Mark), analysis, high-temperature alarm, image enhancement, and capture |
| **3D Analysis** | A 3D height map of the heat |
| **Comparison** | Compare a saved reference board against the live one |
| **Circuit Design** | Lay a board drawing over the thermal picture |
| **Super Resolution** | The 2× enlargement mode |

The **Troubleshoot** tab is the one you will use most; it holds six groups of
controls, each explained on its own page:

- **Circuit Mode** — Short-circuit / Large Current Leakage / Small Current Leakage
- **Temperature Measurement** — Spot / Line / Rectangle / Polygon / None
- **Mark** — Text / Arrow, plus Undo / Redo / Reset
- **Analysis** — Line / Chart analysis
- **High Temperature** — Tracking / Alarm / Highlight
- **Image Enhancement** — Flip H / Flip V / Fixed range / Rapid Diagnostics
- **Capture** — Still / Record / Gallery

## Closing

Close the window as you would any other, or press `q`. The app stops the camera
cleanly on the way out.

---

**Next:** [The picture →](The-picture.md)

*Back to [Home](Home.md)*
