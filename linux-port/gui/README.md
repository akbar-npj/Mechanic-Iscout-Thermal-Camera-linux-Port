# gui — the Qt6 front end

This directory holds the native GUI. It is a *shell*: every pixel it draws and
every string it shows comes from `libdyt`, and the toolkit-independent parts of
that presentation live in `src/view_model.c`, which the OpenCV viewer
(`tools/dytview.cpp`) consumes too. Neither front end owns a formatting rule, so
they cannot drift apart.

| file | what it is |
|---|---|
| `dytqt.cpp` | the Qt6 Widgets application |

## The window

```
MainWindow : QWidget
└── QVBoxLayout (margins 0, spacing 0)
    ├── FrameView   (stretch 1)   the frame, the colour bar, the bar's labels
    └── StatusStrip (stretch 0)   three left-aligned lines
```

`FrameView` is the only class that knows about pixels, and `StatusStrip` the only
one that knows about text layout. Everything above them deals in the session and
the view model.

The window sizes itself to the canvas: `MainWindow::fit_to_view()` is called once
before the window is shown and again whenever the canvas changes size, which is
what makes the first frame — the first moment the sensor's real, zoomed size is
known — fit rather than be cropped. It uses `resize()`, not `adjustSize()`:
`adjustSize()` clamps its result to two thirds of the screen, so the offscreen
platform's 800×600 screen produced a 533×400 window for a 660×400 canvas, and
`--selftest`'s assertion 7 caught it. A window larger than the screen is the
lesser evil; a silently cropped image is not.

### The three status lines

| line | source | example |
|---|---|---|
| 1 | `dyt_vm_status_line()`, verbatim | `mode 1000 \| fusion ir \| 01-iron-red.dat 1/28 \| C \| x2- \| 25 frames` |
| 2 | `dyt_vm_readout_line()`, verbatim | `tool: none (p point, l line, b box, n clear)` |
| 3 | the front end's own | `range auto   \|   FIXTURE  25.0 fps` |

Line 3 carries the two things the view model has no business naming — the frame
rate and the device state — plus the range mode. The mode is formatted by the
engine (`dyt_range_mode_name()`), but it is deliberately **not** folded into
`dyt_vm_status_line()`. That shared line is already ~378 px of a 404 px window at
zoom 1, and `tools/dytview.cpp` draws it into a strip only as wide as the *image*
(256 px at zoom 1), where it is already clipped; extending it would clip further,
and would change another front end's display and its pinned tests for no gain.

The rate is shown only for the states that have painted frames — a `0.0 fps`
beside `CONNECTING` would be a claim about a stream that is not running yet.
`NO SIGNAL` is included on purpose: there the `0.0` *is* the evidence, and hiding
it would leave a frozen picture looking healthy.

The rate is measured from `seq` — the frames the engine processed — not from the
frames painted. A stalled source still returns `FRAME` (it re-renders the last
one), so a paint-driven meter reported a healthy 25 fps over a frozen image,
which is the exact lie the `seq`-driven meter stops telling.

### The device state

`next_live()` returns `WAIT` both before the first frame *and* for the whole
start-up filler (`frame_source.c:271`), so the source alone cannot tell "not
connected yet" from "connected, still warming up". The state is therefore decided
from the session's own `seq`/`ready` — the same scalars-only snapshot `next_live()`
takes internally, so it costs nothing.

| state | when | canvas | line 3 |
|---|---|---|---|
| `Fixture` | not `--live` | image | `FIXTURE  <fps> fps` |
| `Connecting` | `--live`, bring-up still running | `connecting to camera…` | `CONNECTING` |
| `NoDevice` | `--live`, bring-up failed | `no camera found (see stderr)` | `NO DEVICE` |
| `WarmingUp` | `--live`, up, but no real frame yet | `warming up - waiting for live data…` | `WARMING UP` |
| `Live` | `--live`, up, frames advancing | image | `LIVE  <fps> fps` |
| `Stalled` | `--live`, up, but `seq` frozen > 1.5 s | last frame held | `NO SIGNAL  0.0 fps` |

`device_state()` is a pure function of six booleans, so every state is reachable
in `--selftest` with no camera attached — which is the only way two of the six
can be tested at all. `stalled` is checked last, so it can never mask
`Connecting`/`NoDevice`/`WarmingUp`.

**How a stall is found.** The only liveness signal the session offers is `seq`,
the count of frames the engine has processed. A disconnect and a wedge are
identical from there — libuvc handles `LIBUSB_TRANSFER_NO_DEVICE` silently, so
the callback simply stops and `seq` freezes — which is why the state is named
`Stalled` and labelled `NO SIGNAL` rather than for a cause it cannot know.
`StallWatch` fires once `seq` has not moved for 1.5 s (~37 frame intervals at
25 fps), and `ready` gates it, so a warm-up is never a stall however long it
lasts. The last real frame stays on screen; the `0.0 fps` is the evidence.

**The filler is never painted.** Before it has real data the device streams a flat
`0x8000`, which in mode 1000 decodes to a legitimate-looking **238.85 C**
(`display.h`). Painting it would show a real-looking scene that is not one *and*
collapse the colour scale onto it, so `pump::step()` holds the placeholder
instead. Once a frame *has* been painted the placeholder is ignored, so a live
stall keeps showing the last real frame rather than flickering back.

### Zoom and mirror

`dyt_view_transform_map()` states the contract (`display.h:146`): *"the output is
the source magnified by `zoom` and then mirrored"*. `transformed()` transcribes it
in that order, which is what keeps it a direct expression of the contract the
pointer mapping inverts — a front end that scaled or mirrored differently would
put a click on the wrong pixel.

The two orders happen to *agree* for uniform integer magnification (mirroring a
`z`-times block-magnified image and magnifying a mirrored source both send output
pixel `ox` to source `n-1-ox/z`), so this is not a fix for odd zoom. It is
refusing to depend on that coincidence. Assertion 16 pins the mirror actually
happening, driven directly, because nothing yet sets the flip from the UI.

`Qt::FastTransformation` is mandatory rather than a performance choice: it is Qt's
nearest-neighbour, matching `map()`'s integer division and the OpenCV viewer's
`cv::INTER_NEAREST`. Smooth scaling would put the Qt window's pixels somewhere the
shared pointer mapping does not agree with. `QImage::flipped()` is used rather
than the deprecated `mirrored()`.

### Measurement and alarm

The window places measurements with the mouse and picks the tool with the
keyboard, both driving `src/view_model.c` rather than re-deciding anything:

| key | effect |
|---|---|
| `p` / `l` / `b` | point / line / box |
| `n` | no tool, and forget the placed points |
| `a` | arm the alarm, or disarm it if already armed |
| `i` | toggle the isotherm |

One gesture covers all three tools: a press places **both** points, a move while
the button is held moves point 1, and a release ends the drag — so a click leaves
both points on one pixel, which is exactly a point probe, and a drag draws a line
or a box. That rule is `dyt_vm_tool_mouse()`'s, not the window's, and
`view_model_test` pins it. The tool is read from the view's own last snapshot, so
it is the session's tool and there is no second copy to drift.

**The overlay and the pointer share one mapping.** A click maps output→source
with `dyt_view_transform_map()`; the overlay projects source→output with
`dyt_view_transform_project()`. Both are handed the same
`(xform, src_w, src_h, dst_w, dst_h)`, where `dst` is the *transformed* image size
(`img_.size()`), and the widget offset `(kPad, kPad)` is added only at draw time.
Hand-scaling by `zoom` anywhere would put a marker and its click on different
pixels. `FrameView` owns that mapping — and the pointer state with it — because
the hover crosshair needs the pointer position and the temperature plane at paint
time; splitting them across a callback would duplicate the one piece of state
that must not drift.

The canvas draws the frame's hottest and coldest pixels (`H` red, `L` blue, from
`stats`), the temperature under the pointer with a crosshair, and the placed tool
— a ring for a point, a line with endpoint dots, a box with its `min/max/avg/med`
label. The alarm, when it trips, gets a right-aligned badge on the first status
line, the one place with room: the second line can be arbitrarily long and the
image area is where the measurement labels go.

Three strings the viewer used to spell for itself now come from the view model, so
the two front-ends cannot disagree: `dyt_vm_hover_label()` (carrying the `t == t`
guard `dyt_vm_temp()` deliberately omits), `dyt_vm_roi_label()` (a NaN statistic
reads `--`, never a plausible-looking `0 C`), and `dyt_vm_alarm_band()` — the
middle 40 % of the current range with 10 % hysteresis, which is what `a` arms.

Two behaviours are inherited from the viewer rather than corrected. `i` on its own
dims the whole image, because with no alarm armed the isotherm band is `[0,0]` and
every pixel is outside it. And the armed band is fixed at the moment `a` is
pressed, so a camera whose auto-range later settles elsewhere will show an alarm
that no longer matches the scene — the honest consequence of a derived band, and
the reason explicit thresholds are a later task.

## The toolkit decision — Qt6 Widgets

Qt6 was chosen as the toolkit, and within Qt6, **Widgets** rather than Quick/QML.

The deciding fact is what the engine actually produces. `dyt_session_render_rgb()`
hands back a tightly-packed RGB byte buffer, and `src/view_model.c` hands back
finished strings. Widgets consume both directly:

* `QImage(buf, w, h, w*3, QImage::Format_RGB888)` wraps the engine's buffer with
  no conversion, no copy and no format negotiation. Verified, not assumed — see
  the spike below.
* A view-model string goes into a `paintEvent` `drawText` unchanged; there is no
  markup to escape and nothing to re-format.
* The OpenCV viewer's rendering is immediate-mode `cv::rectangle` / `putText`
  calls, which map one-to-one onto `paintEvent`. The layout arithmetic ported
  across unchanged.

QML would instead need an image-provider or texture bridge to get the same
buffer on screen, and would add the QML runtime and scene graph to the
dependency list for no gain — the UI here is a frame, a colour bar, a status
strip and a settings panel, which is exactly what Widgets is for.

The Qt app is an *alternative* front end, not a replacement for the port's rule
that every capability is reachable with no display. `tools/dytview.cpp` remains
the headless-friendly viewer, and both sit on the same `src/view_model.c`.

## How the toolkit was chosen, and the spike that settled it

`dytqt.cpp` began as a spike (task #83) whose job was to answer four questions
with evidence before a window was designed around the answers:

| # | question | answer |
|---|---|---|
| 1 | Does the engine's rendered RGB reach a widget without inventing a conversion? | yes — `QImage` over the engine's own buffer, `Format_RGB888`, no copy |
| 2 | Is 25 fps reachable from a `QTimer` without the frame path stalling? | yes — worst single step ≈0.5 ms (0.5–0.9 ms over 8 runs) against the **40 ms** budget, ~45× headroom |
| 3 | Does the view model's status line drop straight into a Qt widget? | yes — the string goes to `drawText` verbatim |
| 4 | Can the whole thing be verified with no display and no device? | yes — `--selftest` under `QT_QPA_PLATFORM=offscreen` |

The spike also caught a real defect, which is the reason it was worth running
rather than reasoning about: sizing the **window** from the **view's**
`sizeHint()` left the status strip to steal ~22 px from the frame, clipping the
image and the colour bar's bottom label. Every other check still passed — a
frame count and a colour count do not notice a missing row. Assertion 7 now pins
the invariant that `win.view()->height() >= win.view()->sizeHint().height()`, so
the failure cannot return silently.

Two more layout traps were found the same way when the real window replaced the
spike, and both are the kind that only a rendered canvas reveals:

* The window does not follow its layout's `sizeHint` once `resize()` or
  `adjustSize()` has been called on it, so the first frame — the first moment the
  sensor's real, zoomed size is known — grew the *canvas* while the *window*
  stayed at the placeholder size. `MainWindow::fit_to_view()` asks explicitly.
* `adjustSize()` clamps to two thirds of the screen. On the offscreen platform's
  800×600 screen that turned a 660×400 canvas into a 533×400 window: clipped,
  which is precisely what assertion 7 exists to catch. `fit_to_view()` uses
  `resize()` instead.

## Build

The app needs Qt6 Widgets. On a distro that names it `qt6-qtbase-devel`:

```
sudo dnf install qt6-qtbase-devel        # Fedora
sudo apt install qt6-base-dev            # Debian/Ubuntu
```

The Makefile detects it through `pkg-config Qt6Widgets` and sets `HAVE_QT6`;
without Qt6 the target is simply absent and every other target still builds.

```
make build/dytqt
```

`-fPIC` is added automatically — Qt6's libraries are built with
`-reduce-relocations`, so an executable linking them must itself be
position-independent.

## Run

```
./build/dytqt                          # replays the default fixture
./build/dytqt --selftest               # headless check, needs no display
./build/dytqt --palette 5 --zoom 3
./build/dytqt --png /tmp/canvas.png    # save the window and exit
./build/dytqt --live                   # stream from the camera
./build/dytqt --live --vid 0x0bda --pid 0x5840
```

| option | meaning |
|---|---|
| `--fixture PATH` | raw payload to replay (default `testdata/mode1000_256x384_default.raw`) |
| `--width N` | sensor width of the fixture (default 256); with `--live`, the capture width override instead |
| `--palette N` | 1-based palette index (default 1) |
| `--palette-dir D` | where the `*.dat` ramps live (default: search) |
| `--zoom N` | window magnification (default 2) |
| `--frames N` | stop after N frames (default: run until closed) |
| `--fps N` | timer rate (default 25) |
| `--png PATH` | write the canvas here and exit |
| `--selftest` | headless check over the fixture; needs no display |
| `--live` | stream from the camera instead of replaying a fixture |
| `--vid V --pid P` | USB vendor/product id (`0x0000 0x0000` = first matching device) |
| `--format-index N` | UVC bFormatIndex (0 = auto, uncompressed 16-bpp) |
| `--height N` | capture height (0 = auto) |
| `--t-amb C` | LUT ambient for the live path (default 25.0) |
| `--ad-output` | read the flat 256×192 raw-AD frame; default is the device's own 256×384 dual-half frame |

`--live` contradicts `--fixture` and `--selftest` — they ask for two different
sources — and the contradiction is refused rather than silently resolved one way.
`--width` is the one flag that means the same thing in both modes: the sensor's
width, whether that width comes from a file or the camera.

The default live path is the device's own **dual-half** frame (256×384, payload
order unneeded), which is the same default `dytview` and `dytrec` ship. `--ad-output`
switches to the raw-AD frame (256×192) by sending `setTinyCOutputADValue` and
reading the flat plane; it is the path the vendor's AD-mode tools use, and is
only there because the port's super-resolution model was recovered against it.

Once the window is up, `p`/`l`/`b`/`n` place and clear measurements, `a` arms the
alarm and `i` shows the isotherm, and with `--live` the window reconnects on its
own while `R` retries immediately — see "Measurement and alarm".

`--selftest` runs the same code path the window does, under the offscreen
platform plugin, and asserts on the result rather than leaving a human to look
at a window. It is wired into `make check`, so the GUI is part of the CI gate
and not a thing that is only ever run by hand.

```
$ ./build/dytqt --selftest
  ok   painted the requested 25 frame(s) (got 25)
  ok   the frame path fits the 40 ms budget at 25 fps (worst step 2.8 ms)
  ok   the frame is 256x192 (got 256x192)
  ok   the frame converted to real temperatures (min 31.41 C, max 32.41 C)
  ok   the status line is populated ("mode 1000 | fusion ir | 01-iron-red.dat 1/28 | C | x2- | 25 frames")
  ok   the canvas painted (660x456, 64 distinct colours)
  ok   the canvas is not clipped (660x400, wants 660x400)
  ok   the strip has three populated lines
        line 1: mode 1000 | fusion ir | 01-iron-red.dat 1/28 | C | x2- | 25 frames
        line 2: tool: none (p point, l line, b box, n clear)
        line 3: range auto   |   FIXTURE  766.5 fps
  ok   line 3 names the source ("range auto   |   FIXTURE  766.5 fps")
  ok   the range mode reaches the snapshot (auto -> fixed -> auto)
  ok   the engine names the range modes ("auto", "fixed")
  ok   the fps meter is exact (25.0 fps)
  ok   a filler frame is not a live frame (ready 0, WARMING UP)
  ok   a failed bring-up is a state, not a crash (NO DEVICE)
  ok   zoom 2 is applied to the frame (512x384, hint 660x400)
  ok   the mirror is applied (zoom 2, flip_h, 6x2, ends #0000ff/#ff0000)
  ok   --live contradicts --fixture/--selftest, and is refused
  ok   the capture options reach dyt_capture_opts (1234:5678, fmt 3, h 256, t_amb 21.5, AD)
  ok   the stall watchdog fires on a freeze, not on motion or warm-up
  ok   a frozen stream is NO SIGNAL, and cannot mask the other states (NO SIGNAL)
  ok   the fps meter reports 0.0 on a frozen counter (25.0 -> 0.0)
  ok   the retry backoff doubles to a cap, then gives up (0.5, 1.0, ..., 30.0, stop)
  ok   the tool keys reach the session (line/point/box/clear all route)
  ok   the mouse places and drags through the widget (20,15 -> 45,35)
  ok   the alarm key arms the derived band, then disarms (31.7..32.1)
  ok   the isotherm key toggles the overlay
  ok   the strip reports the measurement ("box (10,10)-(60,50) n=2091")
=== ALL PASS ===
```

Two of these are worth calling out because they are the ones that would otherwise
pass vacuously:

* **Assertion 4** is what makes the rest mean something. Mode 1000's start-up
  filler is a flat `0x8000` → ~238.85 C, so a frame near that value means the
  fixture was replayed *without* being converted. The fixture is ~31.4–32.4 C.
* **Assertion 15** requires `zoom > 1` on purpose: at zoom 1 the geometry
  relation it checks would hold for an untransformed frame too, and would prove
  nothing.

The `766.5 fps` on line 3 is not a bug — `--selftest` paces nothing, so it runs
the 25 frames as fast as it can. Assertion 9 checks the *label* there and
assertion 12 pins the arithmetic instead.

Assertions 17 and 18 cover the live path without a camera: 17 checks that
`--live` is refused when it contradicts the fixture-only flags, and 18 checks
that the capture options actually land in `dyt_capture_opts` — the kind of wiring
that otherwise silently does nothing. Both are driven through the pure rule and
`parse_args`, so no usage text is printed into `make check`'s output.

Assertions 19–22 do the same for the error states, and they are the reason the
stall logic is a pure function rather than inlined in the timer callback: 19
drives `StallWatch` through a freeze, a recovery and a warm-up; 20 checks that a
stall cannot mask `NO DEVICE` or `WARMING UP`; 21 pins that the `seq`-driven meter
reads 0.0 on a frozen counter; 22 pins the backoff schedule. None of the four
needs a camera, and none would be reachable any other way.

Assertions 23–27 drive the measurement UI through the **real widgets**, with
synthesized `QMouseEvent`s and `QKeyEvent`s delivered by `QApplication::sendEvent`
— the tool keys, a press/drag/release placing a box, the alarm arming the derived
band and disarming, the isotherm toggling, and the strip reporting the
measurement. The expected source pixel comes from `dyt_view_transform_map()`
itself, so they pin the *routing* (that a widget coordinate reaches
`dyt_vm_tool_mouse()` with the right `dst` size) rather than the transform, which
assertion 16 already covers. The overlay's *painting* is deliberately not
asserted: a pixel check on a drawn rectangle is brittle, so it was verified by eye
from a fixture render instead.

## Where the frames come from, and on which thread

Both the window and `--selftest` replay a frozen fixture through
`tools/frame_source.c`, which runs the real device-free pipeline
(`dyt_pipeline_resolve/frame`, `dyt_visible_extract`, `dyt_session_process`)
exactly as the live capture adapter does. So the app needs no camera to be
built, run or tested.

`--live` swaps the fixture source for `dyt_frame_source_open_live()`, which
pulls from the session the adapter (`src/session_capture.c`) installs frames
into on libuvc's callback thread. The pump only ever *reads* the session through
its own lock (`dyt_session_snapshot`, via `dyt_vm_grab`), so the widget tree is
only ever touched on the GUI thread — which is why the device path needs **no
queued signal**, a point worth recording because the spike's NOTE assumed
otherwise. The poll model stated in `session.h` already covers the hand-off.
`src/frame_ready.c` exists to *wake* a poller on demand instead of polling on a
timer; it is an optimisation, not a correctness requirement, and is not wired in
yet.

Device *control* is the other thread story: bring-up and teardown run on a
worker and marshal their one result back with `invokeMethod` — not a signal, but
the same idea, and only once per operation rather than per frame. That is about
the device; the frame hand-off above is still the poll model, unchanged.

## Bring-up and teardown

The live path is a single bring-up function, `bring_up_live()`, that either
succeeds completely or leaves the state machine in `NO DEVICE`. It runs on a
worker thread, not on the GUI thread (below). The ordering is load-bearing,
from the canonical sequence in `session_capture.h:21-27`:

1. `dyt_capture_open()` — finds and claims the device.
2. `dyt_session_capture_set_capture()` — **before** `start()`, because the
   adapter reads its capture handle on every frame.
3. `dyt_capture_read_info()` — **after** open, **before** start; the identity
   reads (serial, etc.) only answer cleanly while the device is idle (measured
   2026-09-25).
4. `dyt_capture_start()` — registers the frame callback and begins streaming.
5. `dyt_frame_source_open_live()` — last; it only renders what the adapter has
   already installed.

Teardown is the exact reverse, and is also the failure path: `dyt_capture_stop()`
comes first because it joins libuvc's callback thread, and that thread is writing
into the session through the adapter — freeing either the adapter or the capture
before it stops is a use-after-free. Everything in `tear_down_live()` is
NULL-safe and idempotent, so it unwinds exactly what was created and nothing
else, whether bring-up succeeded at step 5 or failed at step 1.

The window is painted before bring-up: `dyt_capture_open()` and the first
`dyt_capture_start()` can each take seconds — and in AD mode `start()` blocks
~3 s + 200 ms by design (`capture.c:600-615`) — so an unpainted window for that
long looks like a hang. The forced paint covers the ordinary slow-open case;
the wedged-device case is the worker's business below.

### Off the GUI thread

Both `dyt_capture_open()` and `dyt_capture_stop()` can block without bound —
open because every libuvc control transfer in it passes timeout `0` (the bullet
under "What is *not* established"), stop because it joins libuvc's callback
thread. On the GUI thread either one freezes the window with no way out: the
GUI thread is the one blocked, so no `QTimer` can rescue it. Both therefore run
on `DeviceWorker`.

A worker runs one job at a time — a second `open()` while the first is still
inside it would fight for the device. A job is a `std::shared_ptr<Job>` that
carries its own *copy* of the capture options, because a wedged job is
abandoned at exit and by then `run_gui`'s `opts` may be gone. `Job::in` and
`Job::out` are the device handles, moved in and out so exactly one side owns
them at a time. The result comes back with
`QMetaObject::invokeMethod(qApp, …, Qt::QueuedConnection)`, which needs no
`Q_OBJECT` and no moc.

It is a detached `std::thread` rather than a `QThread` for one reason: a wedged
worker must be *abandoned*, never joined. A `QThread` member aborts in its
destructor while still running; a detached `std::thread` simply dies with the
process.

### Reconnect

`seq` freezing is not only a label; it drives a reconnect. The first tick that
`step()` sees `NO SIGNAL` it sets `pm.reconnect` — a flag, so the pump stays a
plain function of the session — and the timer callback turns that into a
teardown job and then a bring-up. The frame source is nulled on the GUI thread
*before* the worker is asked to close it, so the pump can never pull from a
source that is being freed.

If bring-up fails, or a running stream dies, the app retries on its own with
the backoff in `retry_delay_s()`: 0.5, 1, 2, 4, 8, 16, 30, 30 s, then it gives
up and says so. `R` retries at once (it skips the wait, not the teardown); the
window takes `StrongFocus` so the key is delivered at all.

### Closing

`closeEvent` calls `on_close_`, which only marks `quitting` and stops the
timers — the device is handed back *after* `app.exec()` returns, so teardown
never runs re-entrantly inside a close event. (Qt calls `closeEvent` on
`app.quit()` as well as on a user close — verified, not assumed — which is why
nothing downstream distinguishes the two.)

At exit, if the worker is still busy it is wedged inside a libuvc call with no
timeout. It cannot be joined (that would hang the exit) and it cannot be left
to finish (it would call back into Qt after the application is gone), so the
only safe move is to leave immediately — flushing stdio by hand, skipping
static teardown, leaking the session the wedged thread may still be writing
into. Otherwise a final teardown job runs and the exit waits up to 1 s for it,
then abandons it the same way.

## What is *not* established

* **`dyt_capture_open()` has no timeout.** Every libuvc control transfer in it
  passes timeout `0`, which libusb reads as *wait indefinitely*, so a wedged
  device hangs the call without bound. It no longer hangs the *window* —
  bring-up runs on `DeviceWorker`, and so does `dyt_capture_stop()`, which
  joins libuvc's callback thread and can block the same way. What is not fixed
  is the call itself: a worker that never returns is abandoned at exit, taking
  its device handle and the leaked session with it, because there is no way to
  cancel it without a timeout in vendored libuvc. Adding one there was the
  alternative weighed and not taken.
* **The parameter ladder and the device panel.** `src/view_model.c` also provides
  the runtime-parameter arm/confirm machine (`dyt_vm_param_key`) and the device
  panel's rows (`dyt_vm_info`), and `view_model_test` pins both. The window does
  not route keys into them yet, so that UI is unexercised — task #89. `n` will
  then have to reach the parameter handler first and be swallowed while something
  is armed, exactly as the reference viewer orders it. The measurement UI itself
  is wired (see "Measurement and alarm").
* **No view keys.** Palette, unit, range, flip, zoom and fusion are reachable from
  the command line (`--zoom`) but not from the window, so the mirror is still
  unexercised interactively — which is why assertion 16 drives the transform
  directly.
* **High-DPI and scaling.** The window paints at 1:1 device pixels, and the
  pointer math assumes it: at a device pixel ratio above 1 Qt scales the drawn
  image, so a widget coordinate would no longer be an image pixel and a click
  would land off-target. Qt's automatic scaling is untested here.
* **Only a smoke test on a real compositor.** The window was shown and painted
  twelve frames on Wayland to prove the platform plugin loads, a window maps and
  the resized layout settles; that is not a sustained run, and it is not part of
  `make check`. The measurement UI was additionally driven live against the camera
  (a key press armed the alarm and the isotherm, and mouse motion reached the
  pointer handler), but synthetic pointer drags proved unreliable on this
  compositor — the window manager slides the window, so a drag's absolute
  coordinates go stale — and the overlay was verified by a fixture render instead.
