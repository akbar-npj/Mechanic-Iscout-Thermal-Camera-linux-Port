# The picture — how the heat is shown

This page is about everything that changes **how the picture looks** rather than
what you measure: colours, the temperature range on screen, zoom, turning and
mirroring, the imaging mode, and the three circuit-diagnosis modes.

## Colour palettes

A **palette** is the set of colours used to draw temperature. Cold parts might be
black or blue, hot parts white or yellow, and everything in between follows a
ramp. Different palettes suit different jobs: a "rainbow" palette makes small
temperature differences easy to see, while a plain greyscale or "iron" palette
looks more like a normal thermal camera.

**How to change it:**

- Press the number keys **`1`–`0`** to pick the first ten palettes directly.
- Press **`.`** for the next palette, **`,`** for the previous one.
- Click the **Palette** icon on the left rail to open the full chooser. This is
  the only way to reach palettes past the tenth — the keyboard only covers ten.

The current palette's name appears in the status strip (line 1), e.g.
`01-iron-red.dat 1/28`.

### Renaming a palette

The palettes ship under their file names (`Iron2.dat`, `Rainbow.dat`), which are
not always the name you would look for. You can give any palette your own
**display name**:

- Open the Palette chooser and choose **"Rename palette…"** at the bottom, or
- **Right-click any entry** in the chooser to rename that one.

Your new name shows in the chooser and the status strip. The original file name
is never changed — it is kept in the tooltip ("Original file name: …") and is
what your saved preference is remembered by, so a rename can never hide which
file is in use. Leaving the box empty restores the file's own name.

## Temperature unit

Press **`u`** to cycle the unit: **°C → °F → K** (Celsius, Fahrenheit, Kelvin).
The chosen unit is shown in the status strip and beside every reading. The unit
is also a setting in the [Settings](Settings.md) dialog.

## The temperature range, and the colour bar

The colour bar beside the picture maps colours to temperatures. Its two ends are
the frame's own coldest and hottest values.

By default the app is in **automatic** range: the colours always stretch across
whatever is currently in view, so the picture uses the full palette even in a
low-contrast scene. The status strip reads `range auto`.

Sometimes you want to **freeze** the range so colours mean fixed temperatures —
for example, so that the same colour means the same temperature across two
different boards. Press **`t`** to switch between **auto** and **fixed**, or use
the **Fixed range** button in the Image Enhancement group.

### The colour bar and its handles

When the range is **fixed**, two small triangle handles appear on the colour bar.
They sit on the bar's boundaries — the **up** handle at the high end, the
**down** handle at the low end — and they travel with the values they set.

- **Drag the up handle** up or down to raise or lower the highest temperature
  shown.
- **Drag the down handle** to raise or lower the lowest temperature shown.
- Each handle moves only its own end; the other is left untouched.
- The two can never cross — a small gap is always kept between them.
- **Double-click anywhere on the bar** to hand the range back to automatic. This
  is also the way out if you drag the window down to almost nothing.

While the range is fixed, the bar's own scale **stops following the frame**. That
is deliberate: a live camera's hottest and coldest values drift from frame to
frame, and if the scale kept re-fitting itself the handles would slide out from
under your pointer. With a fixed range the bar becomes a steady ruler with your
window as a sub-range of it.

### Rapid Diagnostics

The **Rapid Diagnostics** button (or the **`F`** key) is a one-press way to get a
good range automatically. It looks at the current frame's own hottest and coldest
values, adds a margin, and latches that as a fixed range. It is useful when a
scene is so flat that you would otherwise hunt for a range by hand. Pressing it
again simply re-reads the frame — it does not build on the range it set before.

## Zoom

Press **`+`** to zoom in and **`-`** to zoom out. Zoom magnifies the picture
using nearest-neighbour scaling, so it stays crisp — a magnified thermal image
never invents smooth gradients that are not in the data. A zoom level is shown in
the status strip as `x2`, `x4` and so on.

## Flipping and rotating

- **`h`** flips the picture horizontally (left ↔ right).
- **`H`** (Shift-h) flips it vertically (top ↔ bottom).
- The **Rotate** icon on the rail turns the picture a quarter turn clockwise each
  time you click it.

Rotation is deliberately **quarter turns only**. A quarter turn can be undone
exactly on a pixel grid, so a click on a turned picture still lands on the right
pixel; any other angle would need a smoothing step and clicks could drift.

The **Reset** icon on the rail undoes all of these at once — zoom back to the
default, both flips off, rotation back to straight, and the range back to
automatic. It deliberately leaves the palette, unit and imaging mode alone,
because those are about *how* the picture is rendered, not how it is framed.

## Fit to window and full screen

The picture is drawn at its natural size until the window is bigger than it needs,
and then scales up to fill the extra room, keeping its shape and centring itself.
Just enlarge the window by hand and the picture follows.

Press **F11** for full screen. The picture fills the screen, keeping its shape,
with the leftover margin in black; **F11** again restores the window.

## Imaging modes (Fusion)

The camera has **two lenses**: a thermal one and an ordinary visible-light one.
The **Fusion** chooser decides how they are combined. Press **`f`** to cycle the
six modes, or open the **Fusion** chooser from the rail:

| Mode | What you see |
|---|---|
| **Infrared** | The thermal picture only (the default) |
| **Visible** | The ordinary camera picture only |
| **Edge** | Thermal with the visible picture's edges outlined |
| **Blend** | A 50/50 mix of thermal and visible |
| **PiP** | Picture-in-picture: a visible inset on the thermal image |
| **Edge (black)** | The edge mode with a black background |

The Fusion icon on the rail lights up whenever the mode is anything other than
plain infrared, so you can tell at a glance that you are not on the default.

When thermal and visible are combined, the two images must line up. Use **`[`**
and **`]`** (and **`;`** and **`'`**) to nudge the alignment until the visible
edges sit on top of the thermal ones.

> **Note:** the two lenses are aligned to within the same pixel grid by default,
> but the exact optical alignment has not been measured against a physical target
> — see [Troubleshooting](Troubleshooting.md#things-that-are-not-yet-proven).

## Circuit modes

The **Circuit Mode** group at the top of the Troubleshoot tab changes what the
whole panel means. It is the first control the vendor's own software puts there
for the same reason: it picks the job you are doing.

| Mode | Key | What it does |
|---|---|---|
| **Short-circuit** | `S` | The plain, full-range 2D view (the default) |
| **Large Current Leakage** | `L` | Engages a fixed temperature window, to catch big leaks |
| **Small Current Leakage** | `M` | Returns to the full range and jumps to the 3D Analysis tab |

Only **Small Current Leakage** moves you to another tab, because that is the mode
where the 3D view is the point. The other two leave you on whatever page you were
reading.

> The keys are the **capital** letters `S`/`L`/`M`, because the lowercase `s`,
> `l` and `m` are already taken (save, line tool, markers).

---

**Next:** [Measuring temperature →](Measuring-temperature.md)

*Back to [Home](Home.md)*
