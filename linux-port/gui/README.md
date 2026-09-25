# gui — the Qt6 front end

This directory holds the native GUI. It is a *shell*: every pixel it draws and
every string it shows comes from `libdyt`, and the toolkit-independent parts of
that presentation live in `src/view_model.c`, which the OpenCV viewer
(`tools/dytview.cpp`) consumes too. Neither front end owns a formatting rule, so
they cannot drift apart.

| file | what it is |
|---|---|
| `dytqt.cpp` | the Qt6 Widgets application |

## The toolkit decision — Qt6 Widgets

Qt6 was chosen as the toolkit, and within Qt6, **Widgets** rather than Quick/QML.

The deciding fact is what the engine actually produces. `dyt_session_render_rgb()`
hands back a tightly-packed RGB byte buffer, and `src/view_model.c` hands back
finished strings. Widgets consume both directly:

* `QImage(buf, w, h, w*3, QImage::Format_RGB888)` wraps the engine's buffer with
  no conversion, no copy and no format negotiation. Verified, not assumed — see
  the spike below.
* A `QLabel` takes a view-model string as-is.
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

## The spike, and what it settled

`dytqt.cpp` began as a spike (task #83) whose job was to answer four questions
with evidence before a window was designed around the answers:

| # | question | answer |
|---|---|---|
| 1 | Does the engine's rendered RGB reach a widget without inventing a conversion? | yes — `QImage` over the engine's own buffer, `Format_RGB888`, no copy |
| 2 | Is 25 fps reachable from a `QTimer` without the frame path stalling? | yes — worst single step ≈0.5 ms (0.5–0.9 ms over 8 runs) against the **40 ms** budget, ~45× headroom |
| 3 | Does the view model's status line drop straight into a Qt widget? | yes — `QLabel` takes it verbatim |
| 4 | Can the whole thing be verified with no display and no device? | yes — `--selftest` under `QT_QPA_PLATFORM=offscreen` |

The spike also caught a real defect, which is the reason it was worth running
rather than reasoning about: sizing the **window** from the **view's**
`sizeHint()` left the status strip to steal ~22 px from the frame, clipping the
image and the colour bar's bottom label. Every other check still passed — a
frame count and a colour count do not notice a missing row. It is fixed
(`win.adjustSize()`), and assertion 7 now pins the invariant that
`win.view()->height() >= win.view()->sizeHint().height()`, so the failure cannot
return silently.

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
```

| option | meaning |
|---|---|
| `--fixture PATH` | raw payload to replay (default `testdata/mode1000_256x384_default.raw`) |
| `--width N` | sensor width of the fixture (default 256) |
| `--palette N` | 1-based palette index (default 1) |
| `--palette-dir D` | where the `*.dat` ramps live (default: search) |
| `--zoom N` | window magnification (default 2) |
| `--frames N` | stop after N frames (default: run until closed) |
| `--fps N` | timer rate (default 25) |
| `--png PATH` | write the canvas here and exit |
| `--selftest` | headless check over the fixture; needs no display |

`--selftest` runs the same code path the window does, under the offscreen
platform plugin, and asserts on the result rather than leaving a human to look
at a window. It is wired into `make check`, so the GUI is part of the CI gate
and not a thing that is only ever run by hand.

```
$ ./build/dytqt --selftest
  ok   painted the requested 25 frame(s) (got 25)
  ok   the frame path fits the 40 ms budget at 25 fps (worst step 0.5 ms)
  ok   the frame is 256x192 (got 256x192)
  ok   the frame converted to real temperatures (min 31.41 C, max 32.41 C)
  ok   the status line is populated ("mode 1000 | fusion ir | 01-iron-red.dat 1/28 | C | x2- | 25 frames")
  ok   the canvas painted (404x228, 64 distinct colours)
  ok   the canvas is not clipped (404x208, wants 404x208)
=== ALL PASS ===
```

Assertion 4 is the one that makes the others mean something: mode 1000's
start-up filler is a flat `0x8000` → ~238.85 C, so a frame near that value
means the fixture was replayed *without* being converted. The fixture is
~31.4–32.4 C.

## Where the frames come from

Both the window and `--selftest` replay a frozen fixture through
`tools/frame_source.c`, which runs the real device-free pipeline
(`dyt_pipeline_resolve/frame`, `dyt_visible_extract`, `dyt_session_process`)
exactly as the live capture adapter does. So the app needs no camera to be
built, run or tested.

## What is *not* established

* **The device path.** The fixture produces frames on the GUI thread; a real
  camera delivers them on libuvc's callback thread. That hand-off must become a
  queued signal — calling into the widget tree from the callback thread is
  undefined. This is task #85, and `dytqt.cpp` carries a NOTE at the pump
  marking the seam. `dyt_frame_source_open_live()` is the intended entry point;
  it is not yet wired up here.
* **Interaction.** `src/view_model.c` already provides the pointer→tool mapping
  (`dyt_vm_tool_mouse`) and the parameter arm/confirm machine
  (`dyt_vm_param_key`), and both are pinned by `view_model_test`. The spike does
  not yet route Qt mouse and key events into them, so the tool and settings UI
  is unexercised — tasks #88 and #89.
* **High-DPI and scaling.** The spike paints at 1:1 device pixels. Qt's
  automatic scaling is untested here.
* **Only a smoke test on a real compositor.** The window was shown and painted
  three frames on Wayland to prove the platform plugin loads and a window maps;
  that is not a sustained run, and it is not part of `make check`.
