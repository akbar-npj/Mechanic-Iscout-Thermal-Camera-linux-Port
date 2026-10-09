# Troubleshooting

## Common problems

### The picture says `NO SIGNAL`, or the dot is red

- **`NO DEVICE`** (red dot) — the app could not find a camera. Check it is plugged
  in, and that the udev rule is in effect (see
  [Getting started](Getting-started.md#giving-the-app-access-to-the-camera)).
  Run `lsusb` in a terminal; if the camera is not listed, it is a cable or port
  problem, not the app.
- **`NO SIGNAL`** (amber dot, `0.0 fps`) — the camera was streaming and then
  stopped. The last picture is held on screen so you can see what happened. Press
  **`r`** to reconnect immediately, or wait — the app retries on its own with a
  growing delay.
- **`WARMING UP`** — the camera is connected but has not produced a real frame yet.
  This is normal for the first moment or two. The app deliberately does **not**
  draw the camera's blank start-up frame, because it would look like a real
  picture of a very hot scene (about 239 °C) that is not real.

### The app opens but there is no live picture

With no camera attached, the app replays a saved sample frame so you can still use
it. The status strip says `FIXTURE` and the dot is grey — that is expected, not a
fault.

### Super-resolution is greyed out

It needs a model file (`zoom2.mnn`). Without it the page says `No model loaded` and
the choices are disabled on purpose rather than doing nothing. See
[Super-Resolution](Super-Resolution.md).

### Recording a clip does nothing

Clips need the OpenCV library. If the app was built without it, `v` says so. Also
check free disk space — the app refuses to start a clip with less than 64 MB free
and stops one if space falls below 16 MB, telling you why.

### A write to a camera setting did not seem to take

A camera setting change is confirmed by reading it back, but that read can only
happen once the stream stops — on quit or on a reconnect (`r`). So a value you set
and then walked away from is confirmed only in the terminal output. This is a
limitation of the camera itself. See
[The camera itself](The-device.md#confirming-a-write).

### `pkg-config: command not found` / the GUI was not built

These are build problems, not app problems. See the Troubleshooting section of
[`BUILDING.md`](../../BUILDING.md#troubleshooting).

## Things that are not yet proven

This project is careful to distinguish what it has **verified** from what it has
not. If you are relying on the app for real work, know these:

- **Absolute temperature accuracy has not been checked against a calibrated
  reference.** The software's arithmetic matches the manufacturer's own library
  exactly, but that only means it agrees with the vendor — neither has been
  compared to a laboratory thermometer here. Treat the numbers as
  manufacturer-accurate, not independently proven.
- **The two lenses' optical alignment has not been measured.** The thermal and
  visible images line up on the same pixel grid by construction, and `0,0` is the
  vendor's own default, but the exact physical boresight of the two lenses has not
  been measured against a target.
- **Super-resolution is verified for arithmetic, not judged for looks.** Its output
  matches the vendor's own model to within one least-significant bit — but whether
  2× *looks* better has not been measured, and the "thermal plane" mode has no
  vendor counterpart at all.
- **The circuit-layout overlay is stretched and placed by hand.** There is no
  automatic alignment or homography; you line it up with the X/Y offsets. A layout
  drawn at the sensor's own size is exact.
- **A clip plays at the window's frame rate, not the rate it was recorded at**, and
  cannot be fast-forwarded, rewound or seeked. Clips carry no sound.
- **A saved still or a playing clip has no colour bar and cannot be measured.**
  The measurement tools belong to the live view; measuring a saved still is a
  later task.
- **High-DPI displays are untested.** The window paints at one image pixel per
  screen pixel, and the pointer maths assumes it. On a display with automatic
  scaling turned on, a click could land slightly off-target.
- **The live-only indicators are tested with simulated data, not a real camera.**
  The connection dot's colours and the serial number are exercised without a
  camera attached; the exact moment a real camera's dot changes colour during a
  connect, stall and reconnect has not been observed here.
- **The packages have not been through a formal distribution review.** The `.rpm`
  and `.deb` build and install cleanly, but they are not official Fedora or Debian
  submissions.

None of these stop the app being useful for its main job — finding hot components
on a board. They are listed so you know the edges.

---

*Back to [Home](Home.md)*
