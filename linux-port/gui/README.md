# gui — the Qt6 front end

This directory holds the native GUI. It is a *shell*: every pixel it draws and
every string it shows comes from `libdyt`, and the toolkit-independent parts of
that presentation live in `src/view_model.c`, which the OpenCV viewer
(`tools/dytview.cpp`) consumes too. Neither front end owns a formatting rule, so
they cannot drift apart.

**Naming:** the product is *Mechanic iScout Thermal Camera*. That is what the
desktop entry's `Name`, the window title and the About box say, all from one
`kAppName` constant (which `packaging/check.sh` ties back to the entry). The
binary and the packages stay **`dytqt`**, which is why `Exec=`, `Icon=`,
`StartupWMClass` and every install path still read `dytqt`.

| file | what it is |
|---|---|
| `dytqt.cpp` | the Qt6 Widgets application |

## The window

The shell follows the vendor's Windows app (`ThermalAnalysisSystem.exe`; RE Docs
07, and the manual rendered at `/tmp/pdfx/w-08.png`): a **dark theme**, a **left
icon rail**, a centre column with the canvas and its status bar, and a **right
tabbed control panel**. The theme is one stylesheet (`kDarkQss`), applied in
`run_gui` only — never to `--selftest`, whose geometry assertions are calibrated
against the default look.

```
MainWindow : QWidget
└── QHBoxLayout (margins 0, spacing 0)
    ├── IconRail    (stretch 0)   left rail: Palette / Mark / Rotate / Compare /
    │                             Reset, then Tutorials / Contact / Setting
    ├── centre column (stretch 1, QVBoxLayout)
    │   ├── FrameView   (stretch 1)   the frame, the colour bar, the bar's labels
    │   │   └── GalleryPanel          the saved-items list, an overlay child widget
    │   └── StatusStrip (stretch 0)   three lines: status / readout / state,
    │                                 the last with a connection dot, the
    │                                 camera serial and the fps
    └── ControlPanel (stretch 0)  right panel: QTabWidget
        ├── "Troubleshoot"        Circuit Mode / Temperature Measurement /
        │                         Mark / Analysis / High Temperature / Image
        │                         Enhancement / Capture
        ├── "3D Analysis"         a height-mapped surface of the frame
        ├── "Comparison"          a saved reference vs the live frame
        ├── "Circuit Design"      a board layout overlaid on the frame
        └── "Super Resolution"    Off / Visible plane (2x) / Thermal plane (2x)
```

There is **no menu bar and no toolbar**: the reference has neither, and every
action they carried now has a home on the rail, in the panel, or in the Settings
dialog — see [The chrome](#the-chrome). Every rail button runs the same
`handle_key` the keyboard does, so the rail is a second route to the same
actions — never a second set — and every rail button is `Qt::NoFocus`, because a
focused button would swallow the keys before `keyPressEvent` sees them
(assertion 53b pins it). The rail's own items are described under
[The rail](#the-rail) below; assertion 53g clicks each one and checks the effect
against the session.

### The control panel

`ControlPanel` is the Windows counterpart's right column: a `QTabWidget` whose
**Troubleshoot** tab holds seven checkable `QGroupBox`es — Circuit Mode
(Short-circuit / Large Current Leakage / Small Current Leakage), Temperature
Measurement (Spot / Line / Rectangle / Polygon / None), Mark (Text / Arrow), Analysis
(Line / Chart analysis), High Temperature (Tracking / Alarm / Highlight), Image
Enhancement (the two flips, Fixed range and Rapid Diagnostics) and Capture (Still
/ Record / Gallery). All but Capture and Mark are the reference's own groups, in
its own order; Capture is this port's addition, and Mark is the annotation half
of the reference's Mark buttons. Every group the panel shows is backed by the
engine — there is no longer a tab that waits for its feature. The **3D
Analysis**, **Comparison** and **Circuit Design** tabs sit between Troubleshoot
and Super Resolution, where the Windows panel puts them: see below.

Circuit Mode is the reference's function-mode selector, which the panel needs
for the same reason the reference does: it is the one control that changes what
the rows under it mean. *Short-circuit* is the default and is the plain
full-range 2D view; *Large Current Leakage* engages the fixed window (the
vendor's `Update_fixed_temp(false)`, whose name is backwards — `false` is the
branch that *holds* a window, `true` releases it); *Small Current Leakage*
returns to the full range and moves the panel to the 3D Analysis tab. Only the
third moves the tab: a 2D mode that yanked the user off whatever page they were
reading would be its own bug (assertion 65 pins all three). The keys are the
shift forms `S`/`L`/`M`, because `s`, `l` and `m` are still, line and tracking.

**Rapid Diagnostics** sits beside Fixed range because it is the same control
moved by a policy: it reads the frame's own extremes and latches a window that
brackets them with a margin, so a low-contrast scene gets a range that uses the
whole palette without the user hunting for one. Its arithmetic is the vendor's
verbatim, truncating cast and all (see `dyt_vm_rapid_window()` in
`src/view_model.h`), and pressing it twice changes nothing — the second press
reads the frame again, not the window the first one latched.

**Mark** is the reference's annotation tool — `DRAW_CODE.Label` and
`DRAW_CODE.Arrow` (`CAAnalyzer.decompiled.cs:7159`) — and only the annotation
half of it lives here: the label and the arrow, the two items the reference's
own right-click menu offers (`contextMenuStrip_comment`, `:25667`, which adds
exactly `toolStripMenuItem_label` and `toolStripMenuItem_arrow` — not the six
the plan paraphrased). A Mark is not a measurement, so there is no engine call
behind it and no `DYT_TOOL_*` for it; the list of labels and arrows is the front
end's, held on `FrameView` in **source pixels** for the same reason the
measurement overlay is, so a zoom, a flip or a rotation carries a mark with the
pixel it names. `T` arms the Text tool, `w` arms the Arrow tool, and a click in
the picture asks for the string through `on_mark_text_` (a `QInputDialog` in the
window, a stub under `--selftest`, exactly as `on_about_` is), and the label
lands on the pixel the click names — `widget_to_source()` is `pointer()`'s own
inverse, factored out so a label and a point placed by the same click cannot
disagree. A click outside the picture puts the tool away and places nothing
(the vendor refuses one at `:10250`), and a cancelled prompt leaves the tool
armed so the next click can try again. The chip is a plate with the text, a
leader down to a dot on the pixel, clamped inside the picture by `kMarkInset` so
a label on the last row is pulled back rather than drawn half off the canvas —
the vendor fills a rectangle behind its own label for the same reason
(`M_DarwString`, `:14002`).

The Arrow is a press-drag-release, the vendor's own `panel_but_label` route:
press sets `start_point` (`:10285`), move sets `end_point` (`:10554`), and
release calls `AddArrow` (`:11132`) — guarded by `end_point != (0,0)`, which a
press with no drag never satisfies, so a degenerate arrow cannot be committed.
`AddArrow` stores both ends through `rawToBit` (source pixels) with
`arrow_color = Color.White`, and `DrawUArrowF` (`:18386`) is the geometry: head
length `num = (penWidth-1)*5 + 10` = 10 for a 1px pen, half-width 5, notch
`num/3.5`, guard `Math.Sqrt(…) > 5.0`; the six-point polygon `{start, pointF3,
pointF, end, pointF2, pointF4}` is filled and a 1px centre line is drawn over
it. The port follows that geometry verbatim — a dark 3px under-stroke, a yellow
fill, a yellow 1px centre line — and draws the live rubber band before the
release commits, so the drag is visible while it happens. The head is only
meaningful on a segment longer than itself; below the guard the port draws a
bare stub shaft, as the vendor's `DrawUArrowF` does.

A Mark is a *tool*, so it shares one exclusive button group with the measurement
rows: arming one clears the session's tool and picking a measurement tool clears
the Mark, mirroring the vendor's single `cursor_code`. That is why the Mark rows
are a separate group *box* but not a separate button group — two groups would
have needed the `None` row unchecked while the Mark row was checked, and Qt
refuses to uncheck the checked button of an exclusive group. The rail's Mark
button has its own right-click menu (`mark_menu()`), the reference's
`contextMenuStrip_comment` route — a left-click on the rail button re-arms the
last active tool, a right-click offers the two kinds and, below a separator, the
history's three actions. Assertion 68 pins the Text path: the row, the key, the
placement pixel, the painted chip (by difference against the same canvas with no
mark), the disarm, the off-image refusal, the cancelled prompt and the one-tool
rule in both directions. Assertion 69 pins the Arrow path: the row and the key,
the drag's stored endpoints, the live rubber band, the head painted
(before/mid/after, by pixel difference), the no-drag refusal, and the rail menu's
policy, popup, leading two items and key routing.

**Undo / Redo / Reset** take a mark back and put it again. They are the port's
own addition — the vendor's `contextMenuStrip_comment` offers no way to remove a
comment — and they live on the canvas with the marks. `mark_order_` holds the
*kind* of every live mark in placement order, so an undo knows which of the two
lists the last mark is the last of without the two lists needing a shared
container (which would have broken every accessor assertions 68 and 69 read);
`mark_redo_` holds the undone marks themselves, most recently undone first,
because undoing popped one off its list. A new placement discards the redo
branch, the ordinary undo rule, and Reset clears both lists *and* the history —
reset is not "undo everything", because there is nothing to redo back to
afterwards. All three run the same three `MainWindow` methods, reached three
ways: the Mark group's Undo / Redo / Reset rows (through the panel's `act`
callbacks, since a Ctrl chord is not one `handle_key` character — the same
exception the Super Resolution Off row makes), the rail's Mark menu, and the
chords `Ctrl+Z` / `Ctrl+Y` (and `Ctrl+Shift+Z`) / `Ctrl+R`, handled in
`keyPressEvent` before the character read or a chord's control character would be
taken for a binding of its own. The rows grey out with the history — an Undo with
nothing to undo would be a row that does nothing. Assertion 70 pins the model,
the redo, the discarded branch, the rows' enable state and their clicks, the
menu's six entries and their routing, and the four chords.

Each row runs the same `handle_key` its key does (through `ControlPanel::on_key`,
the same one-dispatch rule the menus followed), so a panel button and a key
cannot drift — assertion 50b triggers the Line and Polygon buttons and requires
the session to move exactly as `l` and `o` do. The checkmarks come from the
snapshot in `sync()`, never from the button's own toggle, for the same reason
`sync_actions()` does it: a key the session refused must not leave a button lit.
No control takes focus, for the reason above.

**Polygon** is placed a click at a time rather than by dragging, which is the one
place the tools differ: each click adds a vertex, Enter or the right button
finishes the outline, and Backspace takes a vertex back. The region is measured
from the third vertex on, so there is no "commit" step, and the outline is drawn
with a rubber band from the last vertex to the pointer while it is still open.
The fill is `dyt_measure_polygon()` — an even-odd scanline, so a concave polygon
fills correctly and the winding direction does not matter — and it is the box
tool's convention extended, not a second one: the outline `(x0,y0) (x1+1,y0)
(x1+1,y1+1) (x0,y1+1)` covers exactly the pixels the box `(x0,y0)-(x1,y1)` does
(`measure_test` pins the two against each other). Assertion 24b drives the whole
gesture through the widget and counts the mark colour on the canvas, so the
outline is required to be *painted* and not merely stored.

**Analysis** is the reference's second group, and its two rows are one chart
with two presentations rather than two data sources. The manual never describes
the panel — searching it for "chart" finds only generic circuit-board prose — so
the labels are read literally: **Line** draws the temperature along the placed
line (a slice through space, from `dyt_measure_line`), and **Chart analysis**
draws the *analysis* of that same curve, with the mean and median rules across
it and the peak picked out with its value. Both rows put the line tool up — a
line chart with no line has nothing to show, and a row that only lights up is
the dead control the on-screen-controls rule exists to prevent — so the Line row
is just `l` and the Chart analysis row is `c`, which sets the chart mode and
then routes `l` through the same tool dispatch. Assertion 24c clears the tool
before pressing `c` so that is pinned rather than satisfied by the line the
assertion itself placed.

The plot is a hand-painted `ProfilePlot` (`QPainter`, not Qt Charts — that is a
separate module for one polyline and a few labels). It is *fed* rather than
reading: `MainWindow::sync_actions()` hands it `dyt_session_profile()` once per
painted frame, so the widget holds no session and no lock, and the statistics are
computed on the way in rather than inside `paintEvent` — a sort in a paint is a
sort per repaint. NaN samples are kept in place and break the path instead of
being interpolated across, so a line that leaves the image shows as a gap rather
than a straight segment through nothing. The peak's tie-break is strict `>` —
the same rule `dyt_frame_stats()` and the `H` marker use — so the bin the chart
marks is the pixel the readout names, not a second opinion about it. Assertion
24c pins that: it draws the line along the frame's own hot row so sample *i* is
source x *i*, requires `peak_bin()` to equal the snapshot's `hot_x`, and counts
the marker colour before and after switching to "Chart analysis" to prove the
mode changes what is *painted* and not merely which button is lit.

**Tracking** is a new binding: `m` hides and shows the frame's hottest/coldest
markers (`FrameView::toggle_hot()`), which the reference viewer always draws.
The flag is a canvas one, not a session one — it changes what is painted, not
what is measured. Assertion 50c pins that the key reaches it and that the
drawing actually changes.

**Super Resolution** is the tab the Windows app does *not* have. It is an
addition rather than a port: super-resolution is a recovered capability
(`src/sr.h`, the 2× model `models/zoom2.mnn`), not one of the vendor's controls
— a search of the manual and the installer's resources finds no super-resolution
feature, only the sensor's own "Resolution 256x192". So it gets its own tab
rather than being wedged into the Troubleshoot groups the manual lays out.

Its three rows are `Off`, `Visible plane (2x)` and `Thermal plane (2x)`, routed
through the same dispatch as `z` and `Z`. `Off` is the one row whose key is not
fixed: `z` and `Z` each *toggle their own plane*, so from thermal `z` would
select visible rather than clear. `ControlPanel::sr_off_key()` therefore presses
the key of the mode the session is actually holding, exactly as the menu's Off
item does, and `row()` grew an optional `act` callback for controls whose key
cannot express them.

The page reads the **session**, not the snapshot: `MainWindow::sr_model_loaded()`
asks `dyt_session_sr_capable()`, and the mode comes from `dyt_session_get_sr()`.
That is what lets it be correct before the first frame arrives, when there is no
snapshot to read and the canvas is still the placeholder — which is precisely
when a user wondering why the radios are dead needs the status line to say
`No model loaded`. The session grew that accessor for this: the snapshot already
reports the same fact as `sr_cap`, but it cannot be taken without a frame.
With no model the three radios are disabled rather than left clickable and
silently ineffective. Assertion 53c pins the radios, the status line and the
`Off` row, on both a model-present and a model-absent run.

**3D Analysis** is the tab the Windows panel puts between Troubleshoot and
Comparison. The engine is `src/surface.c` — a pure-C, no-allocation module that
turns the Celsius plane into a height-mapped triangle mesh: `w*h` vertices with
`z` the temperature, finite-difference normals, and `(w-1)*(h-1)*6` indices.
The same module carries the view: `dyt_surface_mvp()` builds an orthographic
orbit matrix and `dyt_surface_project()` applies it, and **both** renderers use
it, so they cannot disagree about where a vertex lands.

The tab's centre is a `SurfaceView`, which owns one model and two renderers.
The GL renderer is a `QOpenGLWidget` (a `QOpenGLShaderProgram`, a VBO/EBO and a
VAO, GLSL 1.10) — the plan's choice, and the one that draws a 97k-triangle mesh
for free. The software renderer is a `QPainter` painter's-algorithm pass over
the same mesh: project, sort the triangles back to front by clip `z`, fill each
with its palette colour shaded by its normal. `SurfaceView` shows GL when a
context comes up and hides it — revealing the software renderer underneath —
when it does not, so the tab is never a dead page. The decision is data, not a
build flag: `using_gl()` is `gl_ready()`, and assertion 60 checks exactly that
equality, so it holds whether or not a context exists. Qt6OpenGLWidgets is
detected and never required (the Makefile's `DYT_HAVE_QT6_OPENGL`); without it
the GL class is not compiled at all and the software renderer is the only one.
Verified both ways: the default build reports `backend GL`, a build with
`QT6_GL_LIBS=` forced empty reports `backend software`, and both pass.

Colour comes from the **active palette**, through the same `dyt_palette_index()`
lookup the canvas uses, at the same display range the snapshot resolved — so a
peak reads the same colour in the surface as in the 2D view. The mesh is
decimated to at most 96x96 (stride-subsampled, both ends pinned) so the software
renderer stays interactive. Drag orbits, the wheel zooms, and **Reset view**
returns to the opening camera. Assertion 60 builds a mesh from a live frame,
checks the first and last vertices are the first and last source pixels (so the
height is the frame's temperature, not a fiction), checks every colour against
the palette lookup, drives the camera with a synthetic drag and wheel, and grabs
the software renderer to require a sizeable, roughly *square* patch of many
shades — square being the check that the aspect term in the matrix still keeps
the mesh square in a wide viewport.

The page's **Height** group is the Windows page's pair of buttons, with its own
labels. `Morphological Change` (the vendor's default, `TempWidthType = 0`)
normalises the mesh's height across the **display window**, so the landscape is
stretched to the same range the canvas is, and data outside the window saturates
into a plateau exactly as the vendor's own clamp makes it. `Color Changes`
normalises across the **frame's own extremes** and leaves the window to the
colour. Only the height differs: the vendor's renderer takes the colour from the
window-normalised index in both branches, and so does this one.

Two consequences worth knowing. The modes are indistinguishable while the range
is auto — with no window engaged the two ranges are the same numbers — which is
also true of the vendor, whose `is_temp_width` is false until a window is set.
And the *normals* have to be computed on the same field the shape is drawn with,
so `dyt_surface_vertices_range()` takes the range rather than deriving it and
`dyt_surface_hnorm()` is the one definition of a vertex's height, used by both
the engine and the widget — a surface shaded for a different scaling would light
as though it were a different landscape. Assertion 67 is the one that separates
the two: it sets a window a fifth of the frame wide, counts the vertices clamped
to exactly 0 or 1, and requires the shape mode to clamp most of the scene while
the colour mode clamps almost none, that every drawn height is inside [0, 1],
and that the `P`/`C` keys were not folded into the point tool and chart analysis
they shadow.

**Comparison** is the tab the Windows panel puts between Troubleshoot and
Circuit Design. The engine is `src/compare.c` — a pure-C, no-allocation module
that takes two Celsius grids (a reference saved still + the live frame) and
produces a signed difference grid, per-pixel stats, and a 50/50 blend preview
reusing `fusion.c`'s `(a + b + 1) / 2` rounding. The tab is the view; the
pump (which owns the session and the per-frame temps) is the model: it loads
the reference still through `dyt_frame_source_open_still` +
`dyt_frame_source_temps` — the same pipeline a gallery still uses — and runs
`dyt_compare_stats()` on every painted frame, pushing the path and the
formatted stats into `ControlPanel::sync_compare()`. The "Load reference…"
button opens a file dialog (or `--reference PATH` loads one at start-up); a
threshold spin box sets the "beyond threshold" count; "Clear reference"
drops it. A size mismatch (the reference is a different resolution than the
live frame) leaves the stats box empty rather than handing the engine a
mismatched pair. The rail's **Compare** button is now live (it was greyed
while the engine waited) and switches the panel to this tab — assertion 53g
flipped from "Compare is disabled" to "Compare switches the tab".

**Circuit Design** is the last of the Windows panel's tabs. It overlays a board
layout image on the thermal picture, so a hot component can be named off the
layout rather than off a bare blob. There is no gerber/CAD parser in scope — the
layout is an image the user loads ("Load layout…" opens a file dialog, or
`--layout-image PATH` loads one at start-up). The image lives on **`FrameView`**,
not on the panel: `set_layout()` / `clear_layout()` / `set_layout_align()` /
`set_layout_opacity()` / `set_layout_shown()`, and a private `draw_layout()`
composites it in `draw_content()` over the base picture and *under* every other
overlay, so the colour bar, the H/L markers and the info panel all stay readable
on top. The page only drives those — `ControlPanel::on_load_layout_`,
`on_clear_layout_`, `on_layout_changed_`, with one free `push_layout()` that both
`run_gui` and the selftest wire, so the panel→canvas contract is the one under
test. Loading an image turns the page's show toggle on, so a load that happened
while the toggle was off cannot look like a failed load.

The align offset is in **drawn-image pixels**, not sensor pixels: the layout is
scaled to the drawn picture, so an offset in sensor pixels would silently change
size when the user zoomed, which no other control does. The ±40 range is the
vendor's fusion alignment limit (`DYT_FUSION_ALIGN_MAX`) and the clamp is
`dyt_fusion_clamp_align()` — the very function the canvas applies — so the spin
box and the canvas cannot disagree about what "in range" means. On the live frame
the layout also gets the view's **rotation and mirrors** (the same `QTransform`
primitives `transformed()` uses for the frame, in the same order), so it turns
with the thermal content instead of staying put while the picture turns under it;
a saved still or clip already has its transform baked in, so its layout is drawn
as loaded.

Assertion 61 pins the tab's name and place, the spin's ±40 clamp, the
panel→canvas push, that the overlay holds the image, the composite itself, and
that the layout follows a quarter turn. The composite is asserted by *difference*
against the same canvas rendered with no layout: a 256×192 layout with one opaque
stripe must appear where the layout puts it, move by exactly the offset, and —
after `dyt_session_rotate()` — reappear as a horizontal band along the top. A
loaded image that changed no pixel would look identical to a working one in a
screenshot of the page, which is why the check is a pixel difference rather than
a "the widget is not null". The probe hides the hot markers and the tool first
(and restores them), so a marker cannot sit on the probe and fail it for the
wrong reason.

A `QTabWidget` whose tabs do not fit hides the overflow behind scroll arrows —
the "control the user cannot reach" failure, and a live risk here because the tab
count only grows. The tab style therefore carries `font-size: 9px` (matching the
rail and the panel rows), and assertion 53d measures the fit — it is the one
assertion here that runs **themed**, because the fit is a property of the
stylesheet's padding and font size and the selftest is unthemed on purpose. The
stylesheet is restored immediately so the geometry assertions after it still see
the unthemed metrics they were calibrated against.

The column is sized for the **tab bar**, not for the rows: the Windows panel
carries four horizontal tabs (Troubleshoot | 3D Analysis | Comparison | Circuit
Design) and ours carries those plus Super Resolution, so `kPanelW` is 440 px.
Measured with the tab style above, the five tabs built today want 415 px of the
438 available (assertion 53d prints it) — tight, but the fit is what 53d exists
to check, and it fails loudly on any host whose font metrics push it over. The
vendor's own panel is ~400 px by the same measure, so this is close to the
reference rather than a departure from it. (It was 224 px, sized for a single
Troubleshoot tab; that overflowed as soon as the second tab landed — the two
wanted 239 px of the 222 available — and assertion 53d is what caught it.)

Its **height** is the same concern turned vertical, and it is subtler. Each
scrollable page sits in a `QScrollArea` with `widgetResizable(true)`, which
resizes the page to the viewport — so a page taller than the viewport is not
scrolled, it is *squeezed*, and the last group is clipped with no scrollbar to
say anything is missing. And `QScrollArea::sizeHint()` reports a small default
rather than its widget's, so a window sized from it comes up short and hands the
page a viewport it cannot fit in. `ControlPanel::sizeHint()` therefore asks the
*page's own layout* for what it needs (unwrapping the scroll area only when the
page is one — the 3D Analysis page is not, because a scroll area would eat the
wheel that zooms the view); the window's hint is the max over its columns, so
covering the panel is what makes the window tall enough. That was a real bug —
the Capture group was cut off at the bottom of the window — and assertion 53h
pins it, on the panel's own hint rather than the window's, because the canvas can
be tall for reasons of its own (a super-resolved frame, a zoom) and would
otherwise hand the panel the room it needed while the panel was still
under-reporting. `minimumSizeHint()` deliberately *doesn't* ask for the content's
height: the panel may be squeezed, and the scroll area is then what keeps the
lower groups reachable — a window that could not be made smaller than its tallest
page could not be made to fit a small screen either. 53h checks both halves.

### The rail

The rail's eight items split three ways: those that act on the session, those
that open a dialog or a popup, and the one whose engine has not landed.

The **top group** acts on the picture. **Palette** opens its picker; **Mark**
re-arms the tool the user last had — `sync_actions()` remembers the last
*active* tool, so `n` clearing the tool does not erase it, and Mark brings it
back rather than always choosing Spot; a *right-click* on the Mark button opens
the two-item popup (Text / Arrow, the reference's `contextMenuStrip_comment`),
so the rail's Mark button is the one place the two kinds come from without the
keyboard; **Rotate** turns the picture a quarter
clockwise per click; **Reset** calls `dyt_session_reset_view`, one session
operation rather than five GUI setters, so a caller cannot forget one of the five
things a reset undoes (zoom to the floor, both mirrors off, the rotation back to
0, range back to AUTO — palette, unit and fusion are deliberately left alone,
since they are how the picture is rendered rather than how it is framed).
**Compare** switches the control panel to the **Comparison** tab, where the
two-board difference engine (src/compare.c) lives — the reference still,
the threshold, and the difference stats. The button was greyed while the
engine waited; it is live now that the engine has landed.

**Rotate** is a session call rather than a GUI flag, because the rotation is part
of the transform the pointer mapping inverts — a copy kept in the window would
let the picture and the clicks disagree. That is also why it is a *quarter turn*
and not a free angle: a quarter turn is the only granularity an integer grid can
invert exactly, so a click on a turned picture still names one source pixel
rather than depending on a resample filter. The button is checkable and lit while
the rotation is not zero, which says "the picture is turned" at a glance and goes
out again when Reset clears it. Assertion 53g clicks it and then checks the
*session* — the rotation advanced, the output geometry swapped its axes, a point
still maps back to the pixel `project()` sent it from, and the canvas's own hint
swapped with it (so `fit_to_view()` re-fits the window rather than leaving the
turned picture cropped) — and assertion 16b renders a rotated frame and requires
every output pixel to be the source pixel `dyt_view_transform_map()` names.
`tools/dytview.cpp` honours the same field and has an `x` key for it, so the two
front ends cannot render the transform differently.

The **Palette picker** is a popup under the button rather than a tab or a dialog,
because the reference's rail item is exactly that: a picker you open, choose from
and dismiss without the picture moving. The first ten entries carry their digit
key and therefore *run* it, so the popup is no more capable than the keyboard for
those; past the tenth there is no key, so those entries set the palette directly.
That is the one place the picker reaches further than the keyboard — it is what
makes the loaded palette set, which is larger than ten, reachable at all. The
menu is rebuilt on each open so its mark is the session's *current* palette, and
it is shown with `popup()` rather than `exec()`: `popup()` returns at once, so no
nested event loop sits on the stack while the picker is open, the pump keeps
painting, and the button can be driven in a test. The menu deletes itself when
dismissed. Assertion 53g pins both pick routes (a digit-keyed entry and a keyless
one), the mark, and that the button opens the picker at all.

The **bottom group** is Tutorials, Contact and Setting. Tutorials opens the same
guide the Help item does. Contact is a read-only information panel — the Windows
app's "Contact us" has nothing to act on — in a `QTextBrowser` so an address can
be selected, which a `QMessageBox` label would not allow. Setting opens the
Settings dialog.

**Settings** presents the four runtime radiometric parameters the `e`/`A`/`R`/`D`
ladder walks as numeric fields. That is not a widening of what the device
accepts: the encoder truncates any value in range (`params.c`), and the
reference's own panel uses editable numeric fields for its thresholds. The spin
boxes are bounded by the *ladder's own* range, so the dialog can offer more
values than the keyboard but never one outside what the keyboard could reach.

It deliberately does not write anything itself. Each row's Send goes through
`FrameView::on_param_send_` — the same callback the ladder's `y` uses — so there
is one write path and the two cannot diverge in what they send, or in what they
report when the device refuses. The dialog adds an input method, nothing else.
One Send per row rather than one for the dialog, because a parameter is armed and
confirmed on its own and a batch would need an all-or-nothing semantics the
device does not have.

It is **non-modal**, and that is the point: sending a parameter is only useful if
you can watch the reading move, and `exec()` would put the reading behind the
dialog (it would also block the pump's timer). The dialog is created on first use
and reused, and re-seeded on every open — the values are the device's, and a write
made since the last open must not leave the fields showing a value nothing holds.
A parameter the device never reported is labelled "(not read)" and starts at the
ladder's first rung, rather than showing a plausible zero.

Seeding has three rules, and assertion 53e pins each with a value that would be
wrong if the rule were missing: an override this session made supersedes the
stored value; a *failed* write does not; and a parameter the device never
reported falls back to the ladder. The same assertion drives the rail's Setting
button, so the wiring is pinned too, and requires the dialog to be modeless.

The dialog has a second group, **Display**, holding the view options the retired
menu bar carried: Unit, Fusion, Zoom, Full screen and the device panel. They live
here rather than in the Troubleshoot tab because the reference's tab has exactly
four groups (Temperature Measurement / Analysis / High Temperature / Image
Enhancement), and this dialog is the reference's own catch-all Setting panel —
the same reason it is the rail's catch-all item. Palette is deliberately absent:
the rail's Palette item *is* its popup picker, and two routes to one list would be
one more than the reference has.

Each Display control routes the same way every other control does: through
`handle_key` when the action has a key (`+`/`-` for zoom, F11, `d`, `r`, `?`),
and to the session when it is an absolute choice no key can express (Unit,
Fusion — the same split the menu bar made). Like the parameter rows, they are
re-seeded on every open, so a unit changed from the keyboard or a full screen
toggled with F11 cannot leave the dialog showing the other one. Assertion 53f
pins all of it, including that the full-screen checkbox *presses F11* rather than
setting the flag itself, which is what keeps the window's `fitted_` bookkeeping
in step.

`FrameView::render_canvas()` draws the canvas's content at the canvas's own
size, in canvas coordinates, and is what the overlay assertions sample. A
widget `grab()` would be cropped once the rail and panel take their columns —
the offscreen test window is wider than its screen — so an assertion about an
overlay is written against the canvas, not the window (assertions 33 and 34).

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

`fit_to_view()` flushes the posted `LayoutRequest` events (and activates the
layout) before reading `sizeHint()`. The canvas's `updateGeometry()` only
invalidates its *immediate* parent's layout and posts an event; the outer
layout's cached hint for the centre column is not recomputed until that event
runs. Without the flush the window would be sized from the *previous* layout —
invisible while the chrome is small, and it clipped the canvas the moment the
icon rail's fixed width made the chrome non-trivial (assertion 7 again).

### The chrome

The window used to carry a menu bar (`File / View / Measure / Device / Help`) and
two toolbar rows above the canvas. They are **gone**: the reference app has
neither, and every action they carried has a home, so nothing was lost but the
duplication.

| what the menu bar carried | where it lives now |
|---|---|
| Save still / Record clip / Gallery | the Troubleshoot tab's **Capture** group |
| Choose folder… | the gallery's **Folder…** button |
| Quit | the window's close button, and `q` |
| Full screen / Device panel | the Settings dialog's **Display** group |
| Palette, and Next/Previous | the rail's **Palette** popup |
| Unit, Fusion | the Settings dialog's **Display** group |
| Zoom in / out | the Settings dialog's **Display** group |
| Fixed range, Flip H, Flip V | the Troubleshoot tab's **Image Enhancement** group |
| Super-resolution Off / Visible / Thermal | the **Super Resolution** tab |
| Point / Line / Box / None, Alarm, Isotherm | the Troubleshoot tab's measurement and High Temperature groups |
| Retry connection | the Settings dialog's **Device** group |
| Keyboard shortcuts, About | the rail's **Tutorials**, and **About…** in Display |

Quit is the one that is not a button: the reference has no in-app Quit either,
and every desktop window already has a close button. It is documented in `--help`
rather than given a control of its own.

The rail, the panel and the palette popup are a second way to reach the same
actions, not a second set of actions. Every control that has a key runs that key
through `handle_key` — the one dispatch the keyboard uses — so a button runs
exactly what its key runs and the two cannot drift. Assertion 50b pins it: the
panel's Line button moves the session exactly as `l` does.

**No `QAction` carries a shortcut.** Qt's shortcut map consumes a matching key
*before* `keyPressEvent` runs, so a shortcut would fire while a runtime-parameter
candidate is armed and break the ladder's swallow contract — every key but `q`
must be swallowed until the candidate is confirmed or cancelled (assertion 30).
The key's name therefore goes in the control's tooltip, not in a `shortcut`.

The exceptions are the controls that select an *absolute* value no key can
express: **Unit** and **Fusion** in the Settings dialog (the `u` and `f` keys
cycle), the palette entries past the tenth in the rail's popup, and **Reset
Image**. Each is a single session call, so there is no rule for the two to
disagree about.

The checkmarks come from the snapshot, not from the button's own toggle:
`sync_actions()` runs after every painted frame and hands `snap_` to
`ControlPanel::sync()`, so a control whose key was refused — a super-resolution
plane with no model, a write the device rejected — cannot stay lit. It is called
only from `set_frame_status`, never from `set_state_line`: the no-device path's
snapshot is zeroed, and syncing from it would clear every checkmark. A stalled
stream keeps the last real frame's states, which is what the canvas is still
showing.

No control may take focus (`Qt::NoFocus`). A focused button swallows the keys
before `keyPressEvent` sees them, which would silently break every binding the
moment someone clicked one; assertions 53 and 53b pin the policy for the panel
and the rail.

`MainWindow` stays a plain `QWidget` rather than becoming a `QMainWindow`:
`QMainWindow::sizeHint()` does not account for its menu and tool bar heights, so
`fit_to_view()`'s `resize(sizeHint())` would size the window for the central
widget alone and let the bars steal rows from the canvas. The centre column's
`QVBoxLayout` rows count towards `QWidget::sizeHint()` automatically, and so does
the panel's own hint (see [The control panel](#the-control-panel)) — which is why
the retirement did not change `MainWindow`'s class.

`StatusStrip`'s horizontal size policy is `Ignored` for the same reason the old
toolbar rows' was: the picture decides how wide the window is, never the text. A
long status line — a saved file's full path, a device's notice — would otherwise
widen the window the moment it appeared, and the window would jump as notices
came and went. The strip elides each line to whatever width it is given, so
nothing is lost, only shortened.

### Fit to window and full screen

The canvas is drawn at its natural size — the engine's zoomed frame — until the
window is larger than it needs, and scales to fit after that. The scale is
`FrameView::display_scale()`: the smaller of the widget's width and height ratios
against the canvas's `sizeHint()`, clamped to at least 1.0, so it engages only
when there is room and is *exactly* 1.0 at the natural size (a 0.99 from rounding
would resample every pixel of the common case for nothing). `display_origin()`
centres the result and floors it to whole pixels — a half-pixel offset at scale 1
would blur the picture and move the overlay samples by one.

This is a **display** transform, applied after everything the engine does. `img_`
is already the source magnified by `zoom * sr`, and the measurement, marker and
pointer mappings all work in those coordinates; `paintEvent` applies
`translate(display_origin()); scale(s, s)` *after* the engine transform, so
`dyt_view_transform_map/project` never see it. `pointer()` is its exact inverse
(same origin, same scale), which is what keeps a click on the pixel it names —
assertion 55 checks that a click at a scaled position lands on the pixel the
projection put there. At the natural size the origin is `(0,0)` and the scale is
1, so the drawing is pixel-for-pixel what it always was — which is what keeps the
pixel assertions honest, since they compare at the natural size.
Nearest-neighbour, like the engine's own zoom: a thermal picture magnified
smoothly invents gradients that are not in the data.

**F11 is full screen** (`toggle_fullscreen()`), the vendor viewer's binding.
Because the canvas scales to whatever room it is given, full screen needs no
special canvas handling — the picture fills the screen, keeps its shape, and
leaves the margin black; F11 again restores the window. `fit_to_view()` returns
early while fullscreen is on: it runs on every painted frame, and `resize()` on a
fullscreen window is not harmless — the window manager owns that geometry and may
drop the fullscreen hint, and on the offscreen platform `resize()` really does
change the size. The guard sits *before* the `fitted_` cache, so leaving
fullscreen clears `fitted_` and re-fits, in case the canvas changed size (a zoom,
or a super-resolved frame) while it was up. Assertion 49 pins the round trip.

### The three status lines

| line | source | example |
|---|---|---|
| 1 | `dyt_vm_status_line()`, verbatim | `mode 1000 \| fusion ir \| 01-iron-red.dat 1/28 \| C \| x2- \| 25 frames` |
| 2 | `dyt_vm_readout_line()`, verbatim | `tool: none (p point, l line, b box, n clear)` |
| 3 | the front end's own | `● range auto   \|   LIVE   Camera SN: CA09DDC00212  25.0 fps` |

Line 3 carries the things the view model has no business naming — the range
mode, the device state, the camera's serial and the frame rate. The mode is
formatted by the engine (`dyt_range_mode_name()`), but it is deliberately **not**
folded into `dyt_vm_status_line()`. That shared line is already ~378 px of a
404 px window at zoom 1, and `tools/dytview.cpp` draws it into a strip only as
wide as the *image* (256 px at zoom 1), where it is already clipped; extending it
would clip further, and would change another front end's display and its pinned
tests for no gain.

The **connection dot** at the left of line 3 is the Windows app's own indicator
(its status bar carries a green dot before the words "Camera connected"),
coloured by the device state: **green** when the camera is up and streaming,
**amber** while it is coming up or has gone quiet (`Connecting` / `WarmingUp` /
`Stalled` — the dot says "not healthy" without claiming "not there"), **red**
for `NoDevice`, and **grey** on the fixture path, where no camera is expected at
all. The grey matters: green there would claim a camera that is not attached,
which is the lie the state word already refuses to tell. `set_conn()` is separate
from `set_lines()` so a caller cannot set the state word without the dot that
qualifies it; line 3's text is inset past it so the two cannot overlap.
Assertion 63 asserts each state's colour *and* finds the dot in a grab of the
strip — a colour function nothing calls would pass a colour-only check.

The serial segment — `Camera SN: <serial>`, the pair the Windows app's own
status bar carries — is the **module** serial `dyt_sn_str()` produces, not the
decoded user serial, which the info panel shows on its own row. It is drawn only
when the bring-up read found one, so a missing serial is an absent segment
rather than a field of spaces. The value comes from **`FrameView`'s
`dyt_device_info_t`**, which is the same struct the info panel and the Settings
dialog read — not from a session field. That is a deliberate departure from the
original plan, which assumed `session.c` performed the bring-up read; it does
not (the front end does, in the idle window between `dyt_capture_open()` and
`dyt_capture_start()`, because the session never touches the capture layer).
Keeping the identity in one place is what makes the status line and the info
panel unable to disagree about which camera is attached, and a teardown's
`clear_info()` clears both. Assertion 62 pins all three: the segment is absent
with no device, present with one, and equal to the info panel's serial row.

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

### Zoom, mirror and rotation

`dyt_view_transform_map()` states the contract (`display.h:146`): *"the output is
the source rotated clockwise, magnified by `zoom` and then mirrored"*.
`transformed()` transcribes it in that order, which is what keeps it a direct
expression of the contract the pointer mapping inverts — a front end that scaled,
turned or mirrored differently would put a click on the wrong pixel.

The magnification and the mirror happen to *agree* for uniform integer
magnification (mirroring a `z`-times block-magnified image and magnifying a
mirrored source both send output pixel `ox` to source `n-1-ox/z`), so this is not
a fix for odd zoom. It is refusing to depend on that coincidence. The rotation
does not commute with either, which is why it goes *first*, in source space, and
why `map()` undoes it last. Assertion 16 pins the mirror actually happening and
assertion 16b the rotation: both render a small image whose pixels encode their
own coordinates through `transformed()` and require the result to match what
`map()` names, so the two cannot disagree about which way round the picture goes.

A rotation is a quarter turn and nothing else. That is the granularity the
vendor's own Rotate button steps, and it is the only granularity an integer grid
can invert exactly: any other angle needs a resample, and then a click could not
name a source pixel without the render and the mapping agreeing on a filter.
`dyt_view_transform_rotate()` takes a delta in degrees and wraps through the four
quarters (a delta that is not a multiple of 90 is rounded rather than dropped),
and `dyt_view_transform_size()` is what reports the axes swapping — a 256×192
frame is 192×256 at 90°, so a front end that rendered `src * zoom` would put
every overlay in the wrong place the moment the picture was turned.

`Qt::FastTransformation` is mandatory rather than a performance choice: it is Qt's
nearest-neighbour, matching `map()`'s integer division and the OpenCV viewer's
`cv::INTER_NEAREST`. Smooth scaling would put the Qt window's pixels somewhere the
shared pointer mapping does not agree with. `QImage::flipped()` is used rather
than the deprecated `mirrored()`.

### Super-resolution

`z` and `Z` run the recovered 2× model (`models/zoom2.mnn`, through
`dyt_mnn_zoom2`) over one of the frame's two planes and render a 512×384
picture. They are one feature with two planes, so they are tied together by
case the way `h`/`H` are, and turning one on turns the other off — both
describe the same 2× render, so there is no coherent reading of "both":

| key | plane the model upscales |
|---|---|
| `z` | the visible half — the vendor's own plane and arithmetic |
| `Z` | the thermal display grey — the port's documented extension |

Both bindings live in `dyt_vm_view_key()`, beside palette and fusion, so the
OpenCV viewer and the Qt app cannot disagree about which letter does what. The
model is fixed 256×192 → 512×384, so only a 256-wide sensor can use it at all;
a 240/384/640 frame leaves the render alone rather than feeding the model the
wrong geometry. Mode `z` additionally needs a visible half *and* a fusion
pattern that shows it — with the default infrared pattern there is nothing on
screen for it to upscale, so it is inactive there rather than doing invisible
work.

The **Super Resolution tab** is the on-screen route to both keys, and the third
place the state is reported — see "The control panel" above for why it reads the
session rather than the snapshot and why `Off` needs a key of its own.

The **factor is carried in the view transform**, not by the window: `sr` joins
`zoom` (`display.h`), and `map`/`project`/`size` use `zoom * sr`. The window
still hands them the native source size and the transformed destination size,
so a click at 2× maps back to the native pixel with no arithmetic in the GUI —
thermometry and measurement stay in native pixels, which is the invariant the
2× must not break. Because the engine renders *before* the window snapshots, the
factor the snapshot reports is the one the render just used, so the mapping and
the picture agree; the one case where they can differ is the failed-allocation
fallback noted under "What is not established".

Feedback is deliberately doubled up, because a key that appears to do nothing
is the one thing this feature must not be:

* the **status line** carries `sr:visible x2` / `sr:thermal x2` while it is on,
  and `sr:<name> (inactive)` when it is selected but cannot take effect;
* a **transient notice** says which of those it is and, when it is refused or
  inactive, the one thing to change — `dyt_vm_sr_notice()` builds the wording
  from the snapshot, so it cannot drift from the render's own refusal. The
  status line stays silent while the mode is off, so without the notice a
  refused `z` would be completely invisible.

Super-resolution is optional at every layer, and the app reports which layer is
missing rather than pretending. At start-up `setup_super_resolution()` finds the
model (`--model`, else the tree or the installed data dir), loads it, and hands
the session `dyt_mnn_zoom2` as its upscaler; a build without MNN, or a host
without the model, leaves the session with no upscaler, and the SR keys then
refuse with `super-resolution: no model loaded`. The About box reports the same
state — "built with: MNN" says a runtime is linked, which is not the same as a
model being loaded, so it says which separately. An explicit `--model` that
cannot be read is refused outright rather than falling through to the search: a
model is not interchangeable the way a directory of palettes is.

Two things the 2× reaches beyond the picture. A still's embedded PNG is the 2×
picture while the DYT container keeps recording the **native** geometry and the
raw payload, so a vendor tool still re-renders from the raw data — the best of
both. And the isotherm overlay goes through
`dyt_vm_apply_isotherm_scaled()`, which samples the temperature plane each
image pixel was magnified from; the unscaled pass would refuse at 2× and
silently dim nothing.

### Measurement and alarm

The window places measurements with the mouse and picks the tool with the
keyboard, both driving `src/view_model.c` rather than re-deciding anything:

| key | effect |
|---|---|
| `p` / `l` / `b` / `o` | point / line / box / polygon |
| `n` | no tool, and forget the placed points and the polygon outline |
| `T` / `w` | Mark: arm the text tool / the arrow tool (a second press puts it away) |
| Ctrl+Z / Ctrl+Y / Ctrl+R | Mark: undo / redo / reset |
| Enter / right button | finish the polygon outline |
| Backspace | take back the last polygon vertex |
| `c` | chart analysis: annotate the line's profile (picks up the line tool) |
| `a` | arm the alarm, or disarm it if already armed |
| `i` | toggle the isotherm |
| `m` | show or hide the hottest/coldest markers |
| `d` | show or hide the device panel |

The runtime-parameter keys and `r` (retry) and `q` (quit) are routed *before*
these and are case-sensitive, so they are documented separately — see "Runtime
parameters and the device panel" below.

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

### Runtime parameters and the device panel

The four radiometric parameters the device stores — emissivity, ambient,
reflected, distance — are the only thing the port ever *writes* to the camera.
A parameter key only **arms** a candidate; nothing is sent until the user
confirms, because the device applies these immediately and a stray keypress
would visibly change the reading.

| key | case | effect |
|---|---|---|
| `e` | lowercase | arm emissivity; pressing it again advances the ladder |
| `A` | Shift | arm ambient |
| `R` | Shift | arm reflected |
| `D` | Shift | arm distance |
| `y` | either | send the armed candidate |
| `n` | either | cancel (also the measurement "no tool" key when nothing is armed) |
| `Esc` | — | cancel |
| `d` | lowercase | show or hide the device panel |
| `q` | lowercase | quit — never swallowed, even while armed |
| `r` | lowercase | retry the device (#87) |

**The letters are case-sensitive, and that is the whole difficulty.** The
reference viewer binds `e`/`A`/`R`/`D` as raw ASCII, so `A` is ambient and `a`
is the alarm, `D` is distance and `d` is the panel, `R` is reflected and `r` is
retry. `QKeyEvent::key()` returns the uppercase code for *both* cases, so the
window reads `e->text()` — which carries the real case — and only falls back to
the folded key for events that have no text (a synthesized `QKeyEvent`, which is
what `--selftest` sends, so assertions 23–27 keep working). `Esc` is translated
to 27 explicitly, because `Qt::Key_Escape` is `0x01000000`, not 27, and
`dyt_vm_param_key()` tests for 27.

The ladder, the arming rule and the "every other key is swallowed while armed"
rule are `dyt_vm_param_key()`'s, in `src/view_model.c`, pinned by
`view_model_test`. `FrameView` owns the arming state and draws the confirmation
overlay; `run_gui` owns the device and reaches it through a
`std::function<bool(type, value)>`. The split is the same one the pointer
handler follows: the state that is drawn and mutated lives with the canvas.

**The write runs on `DeviceWorker`, not the GUI thread.** It is the third
`Job::Kind`, alongside bring-up and teardown. `dyt_capture_set_param()` is two
control transfers of up to 1000 ms each plus a 250 ms settle, so it is ~250 ms
typical and ~2.25 s worst case — and a user who confirms a write should not
watch the window stop responding. The job *borrows* the capture handle rather
than owning it, which is safe only because the worker runs one job at a time and
every teardown is posted through the same worker: `reconnect()` and
`post_bringup()` both refuse while it is busy, so nothing can close the handle
under a write. A confirm pressed during a bring-up or teardown is therefore
**refused**, not queued, and the candidate stays armed so it can be confirmed
again — the status strip says `device busy; try again`. The result comes back on
the GUI thread through the same `invokeMethod` the other jobs use, updates the
panel's override table only on `rc == 0`, and is printed to stderr as well
(`dytqt: set emissivity = 1.00 -> rc 0`), which is what a live run's transcript
shows.

**The device panel** (`d`) lists the module serial, the decoded user serial, the
four stored parameters and the slot count, top-left over the image. Its rows are
`dyt_vm_info()`'s, not the window's. The identity is read once per bring-up,
inside `bring_up_live()` between `open()` and `start()` — the only window in
which the reads answer cleanly — and rides out of the worker in the job's `out`
(so `struct live` gained a `dyt_device_info_t`; it stays trivially copyable
because that is a plain C struct). A value a runtime write superseded is
suffixed `*`, with `(* = set this session)` added to the slot line — but only a
write that returned `rc == 0`, so a rejected write cannot claim to have changed
the device. The `*` therefore means *written, not yet confirmed*, not
*verified*: the confirmation can only arrive later, for the reason below. A
reconnect clears the panel, the override table and any armed candidate, because
the re-opened device's own stored values are authoritative again.

#### Verifying the write

`capture.h` says to verify a write by reading the same index back, and the app
now does — but it cannot do it where the write is made, and that is the whole
story.

`dyt_write_param()` is two OUT transfers with **no status poll**, so it works
while the isochronous stream runs. `dyt_read_param()` polls status register
`0x0200`, and the stream starves that poll. Measured 2026-09-25 on 0bda:5840:
while streaming, every slot read fails (`rc -1`), all four of them, at every
delay tried; stop the stream and all four answer. So the read-back is deferred
to the teardown, which is the first moment the device is idle again and the last
one before the handle closes.

Three measured behaviours shape the code, each worth stating because it would
otherwise look like a bug in the app:

* **The read is not ready the instant the stream stops.** It answered anywhere
  from immediately to about 2 s later across runs. A guessed settle would
  therefore sometimes report a failure that is only impatience.
* **A read taken too early returns the pre-write value.** Measured `127` at
  +500 ms after the stop where the written `128` appeared at +1000 ms, in one
  run out of five; the other four were fresh at +500 ms. A single successful
  read is therefore not proof, and a lone `MISMATCH` could be that lag rather
  than a lost order.
* **While the device still holds an unapplied order it answers nothing at
  all.** Every slot read fails at the transfer level (`rc -1`) for ~8-9 s
  (measured through the app, whose stream stopped with the write still pending).
  A budget shorter than that reports `read-back failed` rather than `MISMATCH`
  for a lost order.

So `verify_writes()` cannot simply wait for an answer and then compare. What
makes a verdict possible is the asymmetry in the middle bullet: reading the
*written* value back cannot be faked, whereas reading anything else may still be
the lag. It therefore polls until every armed slot has shown its written value,
or until its 2.25 s budget runs out, and then judges the last value seen. A
confirmed write costs one pass (~0.3 s); a lost one costs the budget, which
stays short so a normal quit is not held hostage to the ~9 s worst case.

The verdict is `param_raw_matches()`'s, and it compares in the **encoded**
domain. `sendOrder` quantises — emissivity and distance to 1/128, ambient and
reflected to whole kelvin — so a decoded-float compare would call almost every
write a mismatch: `0.80` encodes to `102`, which decodes to `0.796875` and never
reads back as `0.80`. That function is pure, so `--selftest` pins it with no
device (assertion 36); the read itself is the hardware-only half.

Each written slot reports `confirmed`, `MISMATCH`, or `read-back failed (rc N)`
on stderr, and a reconnect also puts a one-line verdict on the status strip
(`write confirmed by read-back` / `write NOT confirmed (n of m)`) for anyone not
watching the transcript. The exit path's teardown budget went from 1 s to 3 s to
cover the wait, though it is only paid when the session actually wrote
something.

The device quirk the harness surfaced is now explained, and it was a measurement
artefact rather than a silent failure: **a runtime order is deferred until the
stream has run ~6.7 s.** Written at 1 s with the stream left running, it lands by
~6.7 s; written at 4.7 s it lands just the same. Written at 1 s and the stream
stopped at 4 s, it is discarded — and the slot still reads `127`, which is what
made it look like "a single `sendOrder` is not always applied". The app's writes
come after the user has been watching, so they always landed; the harness stopped
the stream first. The pair of runs that separates the two: `--wait 1 --post 8`
reads `128`, `--wait 1 --post 2` reads `127`. Nothing in the port fires at that
boundary, and the transfers succeed (`rc 0`) in both cases, so the gate is the
device's and not the port's — and a write made before it still lands later, so
normal use is unaffected.

Two behaviours are inherited from the viewer. The panel is visible by default.
And the write changes the device's *stored* value, not just the host's
thermometry: the host conversion's ambient is fixed at open, so a write to
ambient does not by itself re-scale the picture the window is showing — the
device's own reading changes, which is exactly what the read-back confirms.

### Capture: stills and clips

| key | effect |
|---|---|
| `s` | save the current frame — a `.dyt.jpg` container and a `.png` |
| `v` | start a clip; press it again to stop and finalise |

Files land in `--capture-dir` (default the Pictures folder — see "The gallery"),
named `dyt_<timestamp>.<ext>` by `dyt_vm_capture_name()` so a still and the clip
started in the same second do not collide. `s` is a few milliseconds of work
with nothing to keep between keypresses, so it writes straight away; `v` toggles
the one clip, which the pump then feeds a frame per tick.

**The still is the container, not just a picture.** `dyt_vm_write_still()` (which
predates the window — `dytrec --still` uses it too) writes the DYT container: the
rendered frame as a JPEG, then the APP2 blob describing the payload, then the raw
samples themselves. So a `.dyt.jpg` is a normal JPEG that also carries the
radiometric data — the port's own `dyt_dyt_read()` parses it back (rc 0, blob
1656 B, 196608 raw bytes, geometry 256×192 total 384 flags 0x1 via the `DXT1`
extension record). The `.png` is the same frame for a viewer that knows nothing
about the format. The still is rendered *before* either file is opened, so a
frame that cannot be rendered does not leave half a still behind.

**The clip shares `dytrec`'s writer.** `tools/recorder.h` is the one OpenCV mp4
writer, extracted so the app and `dytrec` cannot drift; the app opens it at the
window's `--fps`, writes the same RGB buffer the canvas is painting, and closes
it to finalise. A frame whose dimensions differ from the clip's is refused rather
than written, because an mp4's dimensions are fixed at open. The whole thing is
OpenCV-gated: without OpenCV the target is still built, and `v` reports
`cannot record: built without OpenCV` rather than failing silently.

**A recording is visible while it runs.** The strip's last line carries a `REC
0:07  175 frames` badge (`dyt_vm_rec_label()`), on its own rather than as part of
a line, because the transient notices come and go and a recording must not. The
outcome of each keypress is a notice like every other action — `saved
dyt_…dyt.jpg + .png`, `recording dyt_….mp4`, `clip stopped: … (n frames)`.

**The disk guard is a guard, not a hope.** Starting a clip needs 64 MB free and
a running one is stopped if free falls below 16 MB, checked every 25 frames
(`dyt_vm_disk_room()` on `statvfs`'s `f_bavail * f_frsize` — the space a
non-root user can actually use, not `f_bfree`). The refusal names the reason —
`cannot record: /path is not writable` or `only N MB free, need 64 MB` — because
"recording failed" with no cause is the kind of message that makes a user retry
the same thing.

The state lives in the pump (`CaptureCtl`), and the two keys reach it through
`on_still_`/`on_record_` — `std::function` callbacks like the parameter ladder's,
for the same no-moc reason. `--selftest` drives both callbacks against a
temporary directory and looks at the files that land.

### The gallery

| key | effect |
|---|---|
| `g` | show or hide the list; hiding also returns to the live view |
| `↑` / `k`, `↓` / `j` | move the highlight (wraps at both ends) |
| `Return` / `o` | open the highlighted entry |
| `x` | export the highlighted still as a PNG |
| `space` | pause or resume a playing clip |
| `Esc` | close the list, or stop a playing clip, and return to the live view |

The list is scanned from the same directory `s` and `v` write to, newest first,
so it shows what this session and earlier ones saved. While it is up it has the
keyboard: any key it does not bind is swallowed rather than reaching the tool,
alarm or parameter bindings — a key that moved the highlight must not also
change the tool. A playing clip is the one state where the list is *not* up
(opening an entry hides it), and `space`/`Esc` reach it there.

**The panel is a real widget, not paint inside the canvas.** It used to be drawn
in `FrameView::paintEvent`, *under* the display transform, from a code path the
no-frame and degenerate-geometry early returns skipped — so it scaled with the
picture and could fail to appear at all, which is what a fullscreen window
showed. `GalleryPanel` is now a child widget composited in *widget* space: the
transform cannot move it and no paint path can skip it. It also brought mouse
selection with it. A click selects a row, a double-click opens it. Everything in
the panel is `Qt::NoFocus`, because the keys belong to the window's single
dispatch and a focusable list would swallow them the moment it was clicked;
mouse events need no focus, so clicks work while the keys stay routed.

**The header names the folder.** The one place a user can see where the files
they are looking at actually are, and the button beside it (**Folder…**, or
File → Choose folder…) changes it. One directory serves both browsing and
saving, so what was saved is what the list shows; the choice sets the same
`capture_dir` `s`/`v` write to and rides out at exit through the preferences.
The default is the **Pictures folder** — `$XDG_PICTURES_DIR` when it names a real
directory, else `$HOME` when it does, else `.` (`dyt_vm_default_capture_dir()`,
pinned in `view_model_test`). A menu launch has no meaningful working directory,
which is why the default is not `.` any more.

**Opening a still re-renders it, it does not just show a JPEG.** The entry's
container is opened as a frame source (`dyt_frame_source_open_still`) and
replayed through the same pipeline a live frame takes, with the app's current
palette — so what you see is the saved radiometric data, not a picture of it. A
container the vendor wrote has no geometry record; then the app's `--width` is
the fallback, and if that does not resolve either, the entry is reported as
unopenable rather than shown wrong. The render happens in a throwaway session,
so the live stream (if any) is never disturbed: the still is drawn as an
*override* over the running pump, and closing the gallery simply clears it.

**Opening a clip plays it.** `tools/player.h` is the reader, the mirror of the
recorder: the same opaque C seam, the same OpenCV-only gate, and the opposite
BGR↔RGB swap — `player_test` writes a colour-skewed frame through the writer and
reads it back, so the two swaps cannot silently drift into a red/blue exchange.
The window's half is a `PlaybackCtl` beside `CaptureCtl`: the pump advances it at
its per-tick hook, so a clip keeps moving on the WAIT and no-frame ticks — which
is exactly when a live stream has nothing to show and a recorded clip does — and
each frame goes through the same `set_viewing()` override a still uses. The
strip carries an amber `playing <name> 12/50` / `paused` badge on its middle
line. `space` pauses without losing the position; `Esc`, or closing the list,
stops the clip and returns to the live view. Opening an entry that fails (a
truncated clip, a file that is not a clip) keeps the list up and says why on the
strip, rather than hiding the panel over nothing.

**Export writes the pixels you are looking at.** `x` renders the highlighted
still the same way `o` does — the two share one `render_still()` so they cannot
disagree — and writes it as a `dyt_<ts>.png` in the capture directory. That is
the "same still, different palette" case the container exists to make possible.
A clip is refused: a PNG of one frame of it is not what the key means.

The browsing state — the entries, the highlight, whether the list is up — is
`dyt_vm_gallery_t` in the view model, because the rules that matter (wrap at the
ends, keep the highlight on the same *file* across a rescan rather than the same
index) are decisions a front end should not make twice, and they are pinned in
`view_model_test`. The panel is a view of that state, so the keyboard and the
mouse cannot disagree about which row is selected.

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

The **3D Analysis** tab additionally prefers Qt6 OpenGL (`Qt6OpenGLWidgets` and
`Qt6OpenGL`, part of `qt6-qtbase-devel` on Fedora and `qt6-base-dev` on Debian
— so the same install already covers it). It is detected and never required:
when the modules are absent the Makefile leaves `DYT_HAVE_QT6_OPENGL` undefined,
the GL renderer is not compiled, and the tab draws through its software
renderer. `packaging/check.sh` pins that the Makefile and the source agree
about the macro, because a rename on either side would silently downgrade every
host to the software path without failing anything.

```
make build/dytqt
```

`-fPIC` is added automatically — Qt6's libraries are built with
`-reduce-relocations`, so an executable linking them must itself be
position-independent.

Unlike `dytview` and `dytrec`, the Qt app **does** link the MNN runtime when the
build has one: it installs the super-resolution upscaler at start-up, so it is
the one front end that references the seam. With no MNN install the link simply
drops `-lMNN` and the app reports no model — the same shape every other optional
feature takes, and the reason `make check` passes both ways. See
`third_party/README.md` for how to produce the MNN install.

## Run

```
./build/dytqt                          # the fixture, or the camera if it is absent
./build/dytqt --selftest               # headless check, needs no display
./build/dytqt --palette 5 --zoom 3
./build/dytqt --png /tmp/canvas.png    # save the canvas and exit
./build/dytqt --shot /tmp/shell.png    # save the whole window and exit
./build/dytqt --live                   # stream from the camera
./build/dytqt --live --vid 0x0bda --pid 0x5840
```

The default source is the fixture — but that path is **relative to the
checkout**, so an installed binary launched from the menu (cwd `$HOME`) has
none to replay, and would otherwise exit with `testdata/…raw: No such file or
directory`. So the app replays the fixture when it is actually readable and
otherwise uses the camera, which is what someone opening a camera viewer wants
anyway. An explicit `--fixture` that cannot be read is still an error rather
than a silent switch to the device. `--selftest` always opens the fixture and
is unaffected.

| option | meaning |
|---|---|
| `--fixture PATH` | raw payload to replay (default `testdata/mode1000_256x384_default.raw`; the camera is used when it is absent) |
| `--width N` | sensor width of the fixture (default 256); with `--live`, the capture width override instead |
| `--palette N` | 1-based palette index (default 1) |
| `--palette-dir D` | where the `*.dat` ramps live (default: search) |
| `--zoom N` | window magnification (default 2) |
| `--unit N` | temperature unit: 0 = C, 1 = F, 2 = K (default C) |
| `--fusion N` | fusion pattern index (default 0 = infrared only) |
| `--model PATH` | super-resolution model (default: search for `zoom2.mnn`) |
| `--reference PATH` | a saved still (`.dyt.jpg`) to diff against in the Comparison tab (default: none) |
| `--layout-image PATH` | a board layout image to overlay in the Circuit Design tab (default: none) |
| `--sr MODE` | super-resolution: `off` \| `visible` \| `thermal` (default `off`) |
| `--frames N` | stop after N frames (default: run until closed) |
| `--fps N` | timer rate (default 25) |
| `--png PATH` | write the canvas here and exit (implies `--frames 1` unless one is given) |
| `--shot PATH` | write the whole window here and exit — the shell, rail and panel included; use `--png` for the picture alone |
| `--capture-dir D` | where `s` (still), `v` (clip) and the gallery write/read (default the Pictures folder: `$XDG_PICTURES_DIR`, else `$HOME`, else `.`) |
| `--prefs PATH` | preferences file (default `$DYT_PREFS`, else Qt's config location) |
| `--no-prefs` | neither read nor write saved preferences |
| `--selftest` | headless check over the fixture; needs no display |
| `--live` | stream from the camera instead of replaying a fixture (automatic when the default fixture is absent) |
| `--vid V --pid P` | USB vendor/product id (`0x0000 0x0000` = first matching device) |
| `--format-index N` | UVC bFormatIndex (0 = auto, uncompressed 16-bpp) |
| `--height N` | capture height (0 = auto) |
| `--t-amb C` | LUT ambient for the live path (default 25.0) |
| `--ad-output` | read the flat 256×192 raw-AD frame; default is the device's own 256×384 dual-half frame |

`--live` contradicts `--fixture` and `--selftest` — they ask for two different
sources — and the contradiction is refused rather than silently resolved one way.
`--width` is the one flag that means the same thing in both modes: the sensor's
width, whether that width comes from a file or the camera.

An explicit flag always beats a stored preference: the file is loaded first and
the command line is applied over it, so `--unit 1` wins over a saved `unit=0`
without touching the file. `--no-prefs` is what the selftest uses, so a test run
cannot clobber a developer's saved state.

The default live path is the device's own **dual-half** frame (256×384, payload
order unneeded), which is the same default `dytview` and `dytrec` ship. `--ad-output`
switches to the raw-AD frame (256×192) by sending `setTinyCOutputADValue` and
reading the flat plane; it is the path the vendor's AD-mode tools use, and is
only there because the port's super-resolution model was recovered against it.

Once the window is up, `p`/`l`/`b`/`n` place and clear measurements, `a` arms the
alarm and `i` shows the isotherm, `z`/`Z` turn on super-resolution, `s` saves a
still and `v` records a clip, `g` browses what has been saved (and opens a still
or plays a clip; `space` pauses it), `d` shows the device panel, `S`/`L`/`M` pick
the circuit mode, `F` auto-fits the range and `P`/`C` pick the 3D page's height
mode — and with `--live` the window reconnects on its own while `R` retries
immediately — see
"Measurement and alarm", "Super-resolution", "Capture: stills and clips" and
"The gallery". `F11` is full screen, and the picture scales up to fill a window
enlarged by hand — see "Fit to window and full screen".

Everything the keys do is also on the **rail or in the right panel**, and the
rail's **Tutorials** opens a scrollable guide (`F1` and `?` open About instead).
A button runs exactly what its key runs, so the on-screen controls cannot drift
from the keyboard — see "The chrome".

`--selftest` runs the same code path the window does, under the offscreen
platform plugin, and asserts on the result rather than leaving a human to look
at a window. It is wired into `make check`, so the GUI is part of the CI gate
and not a thing that is only ever run by hand.

```
$ ./build/dytqt --selftest
  ok   painted the requested 25 frame(s) (got 25)
  ok   the frame path fits the 40 ms budget at 25 fps (worst step 0.9 ms)
  ok   the frame is 256x192 (got 256x192)
  ok   the frame converted to real temperatures (min 31.41 C, max 32.41 C)
  ok   the status line is populated ("mode 1000 | fusion ir | 01-iron-red.dat 1/28 | C | x2- | 25 frames")
  ok   the canvas painted (1172x969, 64 distinct colours)
  ok   the canvas is not clipped (660x913, wants 660x400)
  ok   the strip has three populated lines
        line 1: mode 1000 | fusion ir | 01-iron-red.dat 1/28 | C | x2- | 25 frames
        line 2: tool: none (p point, l line, b box, o polygon, n clear)
        line 3: range auto   |   FIXTURE  875.2 fps
  ok   line 3 names the source ("range auto   |   FIXTURE  875.2 fps")
  ok   the range mode reaches the snapshot (auto -> fixed -> auto)
  ok   the engine names the range modes ("auto", "fixed")
  ok   the fps meter is exact (25.0 fps)
  ok   a filler frame is not a live frame (ready 0, WARMING UP)
  ok   a failed bring-up is a state, not a crash (NO DEVICE)
  ok   zoom 2 is applied to the frame (512x384, hint 660x400)
  ok   the mirror is applied (zoom 2, flip_h, 6x2, ends #0000ff/#ff0000)
  ok   the rotation is applied (rot 90, 2x3, 0 of 6 pixels off, clockwise yes)
  ok   --live contradicts --fixture/--selftest, and is refused
  ok   the capture options reach dyt_capture_opts (1234:5678, fmt 3, h 256, t_amb 21.5, AD)
  ok   the stall watchdog fires on a freeze, not on motion or warm-up
  ok   a frozen stream is NO SIGNAL, and cannot mask the other states (NO SIGNAL)
  ok   the fps meter reports 0.0 on a frozen counter (25.0 -> 0.0)
  ok   the retry backoff doubles to a cap, then gives up (0.5, 1.0, ..., 30.0, stop)
  ok   the tool keys reach the session (line/point/box/clear all route)
  ok   the mouse places and drags through the widget (20,15 -> 45,35)
  ok   the polygon is placed a click at a time, and its outline is painted (3 clicks, closed, undone, right-button yes, 276 mark px then 0)
  ok   the Analysis chart plots the line's profile and marks its peak (peak bin 1 of hot x 1, chart analysis, picks line yes, marker 0 px then 20)
  ok   the alarm key arms at the threshold field's value, then disarms (-20.0..70.0)
  ok   the isotherm key toggles the overlay
  ok   the strip reports the measurement ("box (10,10)-(60,50) n=2091")
  ok   a parameter key arms its ladder and advances it (arm yes, advance yes, switch yes)
  ok   the case split holds (e/A/R/D yes, r=retry yes, R=reflected yes)
  ok   a key while armed is swallowed and ESC cancels (swallow yes, cancel yes)
  ok   y sends the armed value and a refusal keeps it armed (send yes, keep yes)
  ok   the device panel rows and the override `*` (rows yes, override yes, failed-write yes)
  ok   the device panel is hidden at start-up and painted over the image when shown (hidden yes, fill yes, toggle yes, covers yes)
  ok   the confirmation is painted only while armed (idle 0, armed 7378, cancelled 0)
  ok   q quits and is never swallowed (idle yes, armed yes)
  ok   the read-back compares in the encoded domain (quantised yes, exact yes, kelvin yes)
  ok   a still writes the container and the PNG (wrote yes, 1 + 1, sized yes)
  ok   a still with SR on is written at 512x384 (wrote yes, PNG 512x384)
  ok   the gallery opens a saved still (scan 1, still 0, frame 256x192, temps 31.41..32.41 C)
  ok   a clip starts, takes frames, and stops (start yes, label yes, fed yes, 5 frames, 1 file, gone yes)
  ok   a clip is refused where there is no disk (refused yes, says why yes)
  ok   the capture keys route (still 1, record 1)
  ok   the gallery keys browse, open and export (open yes, move yes, swallow yes, opened 1, hid yes, exported 1, closed yes, back to live yes)
  ok   the gallery is a real widget: up on 'g', painted and still up in fullscreen, names the folder (shown yes, folder yes, fullscreen yes, painted 4760 px, back yes, hidden yes)
  ok   the gallery takes the mouse (rows 2, click selects yes, double-click opens yes)
  ok   a clip plays, pauses and stops (open yes, 8 frames, moved yes, sized yes, magenta 49152 vs 0/0, badge yes, paused yes, resumed yes, stopped yes, refused a still yes)
  ok   the About text names the app and its version (0.1.0), the SR keys, the model state and the shared key list (about yes, guide yes)
  ok   the super-resolution keys route, keep their case and post a notice ('z'->visible, 'Z'->thermal, "sr:thermal x2")
  ok   a 2x render maps a click back to the native pixel (both corners)
  ok   F11 is full screen, and leaving it re-fits the window (entered yes, left yes, back to 1172x969 yes)
  ok   a panel button reaches the session like its key (line yes, polygon yes, clear yes)
  ok   the tracking key hides and shows the extremes (hidden yes, back yes, drawing changed yes)
  ok   the checkmarks follow the frame, not the click (flip h 0 then 1, matched yes / yes)
  ok   the colour bar's range handles sit on the frame's scale, drag the window and reset to auto (geometry yes, painted yes, drag yes, moved yes, auto yes)
  ok   rapid diagnostics latches a fixed window over the frame, by the key and by the row (row yes, mode yes, contains yes, reframe yes, idempotent yes, row-same yes)
  ok   the circuit modes set the range and the view (rows yes, default yes, large yes, small yes, short yes)
  ok   the 3D height modes scale the mesh to the window or to the frame (rows yes, keys yes, shape clamps yes, colour clamps yes, differ yes, in range yes)
  ok   the Mark Text tool places a label at the clicked pixel and paints it there (row yes, key yes, place yes, painted yes, disarms yes, off-image yes, cancel yes, one tool yes)
  ok   the Mark Arrow tool drags an arrow between the pixels and the rail's Mark menu offers both kinds (row yes, key yes, drag yes, live yes, head yes, no-drag yes, menu yes, menu keys yes)
  ok   the Mark history undoes, redoes and resets, by the rows, the menu and Ctrl+Z / Ctrl+Y / Ctrl+R (model yes, redo yes, branch yes, rows yes, menu yes, keys yes)
  ok   the control panel cannot take the keyboard (35 control(s), 0 that would)
  ok   the icon rail cannot take the keyboard (8 button(s), 0 that would)
  ok   the Super Resolution tab reflects the session (mode off, model loaded, plane yes, off yes)
  ok   every control-panel tab fits, with no scroll arrow (5 tab(s), 415 px of 438)
  ok   the Settings dialog opens from the rail, is modeless, and sends what its fields hold through the ladder's own write path (4 row(s), open yes, modeless yes, seeded yes, sent yes, refusal yes, re-seeded yes)
  ok   the Settings Display section drives the session and the window (seeded yes, unit yes, fusion yes, zoom yes, full screen yes, panel yes, retry+about yes)
  ok   the rail's items reach what they claim (28 palette entries yes, mark yes, pick yes/yes, popup yes, re-arm yes, reset yes, rotate yes, refit yes, rotate-reset yes, tutorials yes, compare yes)
  ok   the control panel asks for its content's height (panel 969 of 969, page 943 of 943, shrinks yes, keeps yes, scrolls yes)
  ok   the canvas fits the window when there is room (1:1 yes, grown 1.39x yes, centred yes, back yes)
  ok   a click at a scaled position names the right pixel (1.39x, (511,382) -> (85,64), wanted (85,64))
  ok   the 3D Analysis tab builds a mesh from the frame, colours it from the palette and orbits (tab yes, mesh yes 96x96, height yes, colour yes, camera yes, drag yes, wheel yes, backend yes (GL), paint yes)
  ok   the Circuit Design tab overlays a layout and aligns it (tab yes, clamp yes, push yes, hold yes, paint yes, move yes, rot yes)
  ok   the status line names the camera's serial (absent yes, shown yes, agrees yes)
  ok   the connection dot names the state in colour (colour yes, painted yes)
=== ALL PASS ===
```

The transcript above is abridged — assertions 43–45 (the view keys, the
preferences round-trip and the About key route) are omitted for length; the run
prints them between the gallery keys and the About text.

Two of these are worth calling out because they are the ones that would otherwise
pass vacuously:

* **Assertion 4** is what makes the rest mean something. Mode 1000's start-up
  filler is a flat `0x8000` → ~238.85 C, so a frame near that value means the
  fixture was replayed *without* being converted. The fixture is ~31.4–32.4 C.
* **Assertion 15** requires `zoom > 1` on purpose: at zoom 1 the geometry
  relation it checks would hold for an untransformed frame too, and would prove
  nothing.

The `fps` figure on line 3 is not a bug: `--selftest` paces nothing, so it runs
the 25 frames as fast as it can, and the number is whatever the machine manages
on the run. It differs every time, and so does `worst step` — those two are the
only lines in the transcript above that a re-run will not reproduce. Assertion 9
checks the *label* there and assertion 12 pins the arithmetic instead.

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
assertions 16 and 16b already cover. The measurement overlay's *painting* is
deliberately not asserted for the box and the line: a pixel check on a drawn
rectangle is brittle, so it was verified by eye from a fixture render instead.

**24b** is the polygon, and it is the one that does assert a measurement
overlay's painting — by *counting* the mark colour with the outline placed and
again after `n`, so what is compared is the difference the outline makes rather
than an edge pixel that a one-pixel shift would move. The colour bar is drawn
from a palette that contains pure yellow, which is why the difference is counted
rather than the total. It also drives the whole gesture through the widget:
three clicks, Enter to finish the outline, Backspace to take a vertex back, and
the right button to finish it — including that the right button does *nothing*
to a two-vertex outline, which is not yet a region.

**24c** is the chart, and it pins two different things. The *data*: the line is
drawn along the row the frame's own hot pixel is in and right across the image,
so sample *i* is source x *i* and the peak bin the chart marks has to equal the
snapshot's `hot_x` — a number taken from the frame, not from the chart. The
*painting*: "Chart analysis" has to change what is drawn, not only which button
is lit, so the two modes are compared by counting the peak marker's colour,
which the annotated mode draws and no palette produces. It also clears the tool
and presses `c` on its own, because the row claims to put the line tool up and
that claim is worth nothing if the assertion had already placed one.

Assertions 28–35 cover the runtime-parameter ladder and the device panel, and they
are the reason the arming state lives in `FrameView` rather than in the front end:
they drive the real window with real key events and then read the widget's own
state. 28 pins that a key arms its ladder and that re-pressing it advances the
rung; 29 pins the case split — `A`/`R`/`D` arm their parameters, lowercase `r` is
retry and uppercase `R` is reflected, which is the binding an unconditional fold
would destroy; 30 pins that every other key is swallowed while armed and that
`Esc` cancels; 31 pins that `y` reaches the front end with the armed value and
that a refusal leaves it armed. The write itself needs a device, so the send is
intercepted — what these pin is the routing and the contract, not libuvc.

32–35 are where the panel and the confirmation are checked. 32 builds a synthetic
`dyt_device_info_t` and pins the rows *and* the `*` rule, including that a write
returning `rc != 0` does not mark the stored value as superseded. 33 and 34 assert
that both overlays actually paint, which is the check 23–27 deliberately skip for
the measurement shapes: rather than sampling one pixel of a drawn rectangle, 33
samples three interior pixels that the panel's opaque background must cover, and
34 *counts* the confirmation's fill colour with the candidate armed and without
(`idle 0, armed 7048, cancelled 0`) so an unlucky palette pixel cannot make it
pass on its own. 35 pins that `q` quits and is never swallowed, armed or not. 36
pins `param_raw_matches()`, the verdict the deferred read-back reaches: that
`0.80` matches the device's `102` (and `103` does not) even though `102/128` is
not `0.80`, that an exact step does not tolerate a one-LSB error, and that the
two Celsius types compare as whole kelvin.

37–42 cover capture and the gallery. They write into a `mkdtemp` directory and
look at what landed, which is why the fixture now installs the raw payload: 37
asserts that `save_still()` writes *both* halves — one `.dyt.jpg` and one `.png`,
each over a kilobyte — and that its message starts with `saved `; 38 scans that
directory, opens the still as a frame source and checks it renders 256×192 with
real temperatures (31–32 C, not the filler's ~238.85 C), which is what makes
"opening a still goes through the live pipeline" more than a claim; 39 drives the
`CaptureCtl` state machine through start/feed/stop, checks the indicator is
non-empty while running and empty after, and confirms one `.mp4` was written (or,
in a build without OpenCV, that the refusal names OpenCV — the gate is the
point); 40 points the guard at a directory that cannot exist and pins that it
refuses *and* says why; 41 pins that `s` and `v` reach
`on_still_`/`on_record_` at all; 42 drives the real gallery keys and pins that
`g` opens the list and rescans, the arrows move the highlight, `Return` and `x`
reach their callbacks, a key the list does not bind is swallowed rather than
reaching the tool, opening hides the list, and closing returns to the live view.

57–59 cover the gallery as it is now, and each pins a defect the painted version
had. 57 requires the panel to be a real widget — visible on `g`, still visible
and actually *painted* in fullscreen (a pixel diff, because `isVisible()` alone
would not catch a widget that is up but never drawn), naming the folder, and
hidden on close. 58 synthesizes a click and a double-click on a row and requires
them to select and to open, because the painted list had no mouse handling at
all: a click fell through to the canvas and placed a measurement tool. 59 writes
a clip from a solid magenta frame and requires the canvas to show it — no
thermal palette produces magenta, so counting those pixels in a canvas grab
decides that the clip reached the screen, where a diff against the live frame
would prove nothing, because a clip recorded from this session *is* the live
frame. It also pins that `space` pauses without losing the position and that
`Esc` stops and returns to live. The mp4's playability is pinned twice over:
here, and by `tools/player_test.c` in `make check`, which round-trips a
colour-skewed frame through the writer and back.

43–48 cover the view keys, the preferences, the About box and
super-resolution. 43 drives the
bindings the reference viewer's letters map to — palette digits, `.`/`,`, `u`,
`t`, `h`/`H`, `+`/`-` — through the real window and reads the session back, and
pins that `h` and `H` move *different* axes (an unconditional fold would make
them the same key); the palette count is read from the snapshot rather than
hard-coded, so a change in the ramps cannot silently invalidate the test. 44
writes a preferences file, reads it back, and checks that a command-line value
beats a stored one; it runs under `--no-prefs` so it cannot touch a developer's
own file. 45 pins that `?` and F1 both reach the About action. 46 checks the
About text names the app and the version the package carries — which is what a
stale About box would fail, and, through `-DDYT_VERSION`, what ties the box to
the Makefile's `VERSION` — and that it reports the model state the feature
macros cannot. 47 drives both SR keys and pins that they keep their case, reach
the session, and post the notice that is a refused key's only feedback. 48 pins
the reason the factor is in the transform at all: with a 2× picture, mapping the
two corner output pixels must land on the plane's two corners. It is written to
skip (and say so) on a build with no model, where there is no 2× picture to map.

49–56 cover the window chrome — full screen, the fit-to-window display layer,
and the on-screen controls the menu bar and toolbar gave way to. Each pins a
failure that would otherwise stay invisible until a user hit it, so they are
worth naming one by one:

* **49** drives F11 in and out and requires the window to come back the size it
  went in at, which is what catches the fullscreen guard in `fit_to_view()`
  leaving `fitted_` advanced and the window stuck at the screen's size.
* **50b** clicks a panel button and reads the session back, so the on-screen
  controls cannot become a second front end free to drift from the keyboard.
* **51** flips from a *key*, not the button, and requires the checkmark to match
  the session both times *and* the two frames to differ — so neither a sync that
  never ran nor one echoing the button's own toggle can pass.
* **53 / 53b** require the control panel's buttons and the rail's to be
  `Qt::NoFocus`, which is what stops a clicked button from swallowing every key
  afterwards.
* **53d** requires the panel's tabs to fit its column with no scroll arrow, and
  **53h** requires the panel to ask for its content's height so the last group is
  not clipped — the two directions of "a control the user cannot reach". Both are
  described under [The control panel](#the-control-panel).
* **54** grows the window and requires the canvas to be 1:1 before, scaled and
  centred after, and 1:1 again when it shrinks back; the first half is what keeps
  the pixel assertions honest, since at the natural size the scale is exactly 1
  and the origin exactly `(0,0)`.
* **55** clicks at a scaled position and requires the placed point to be the
  pixel the projection put there. It is the assertion that catches a scale
  applied to the paint but not to the pointer, which would place every marker
  somewhere else while looking entirely plausible.

The last stretch of the parity work — the Windows panel's remaining controls —
added seven more. **64** is the colour bar's two range handles: it reads their
geometry off the widget, finds the hot handle's pixel in a render, drags one and
requires the session to hold the window the drag produced, then double-clicks
back to auto. **65** is Rapid Diagnostics, and its second half is the one that
bites: pressing it twice must change nothing, because the second press reads the
*frame's* extremes and not the window the first one latched — at AUTO those are
the same numbers, so nothing weaker separates them. **66** drives all three
circuit modes and requires each to set the range the vendor's own handler sets,
that only Small Current Leakage moves the panel to the 3D tab, and that the
checkmark follows. **67** separates the 3D page's two height modes by counting
clamped vertices under a window narrower than the frame, and requires every
drawn height to be inside [0, 1] — the vendor's clamp. **68** is the Mark Text
tool, and it is the one assertion here that pins a feature with no engine behind
it: the row and the key, that `T` was not folded into `t` (the range must not
have moved), that the label lands on the source pixel the click names, that the
chip is painted at that pixel's projection — asserted by *difference* against the
same canvas with no mark, so an overlay that was already there cannot satisfy it
— that the tool disarms after a placement, that a click off the picture places
nothing, that a cancelled prompt leaves the tool armed, and that the Mark and the
measurement tools clear each other in both directions. **69** is the Mark Arrow
tool: the row and the key, that `w` arms the Arrow tool and clears the session's
tool (the one-tool rule again), that a drag's stored endpoints are the source
pixels the press and release name (asserted through the same mapping the
placement uses), that the rubber band is live while the drag is in flight and
nothing is stored until the release commits it, that the head is painted at the
head's own projection — asserted by pixel difference before, during and after
the drag, so a head that was never drawn cannot satisfy it — that a press with
no drag places nothing and leaves the tool armed (the vendor's own
`end_point != (0,0)` guard), and that the rail's right-click menu has the
right policy, opens on a sent context event, offers the two kinds first and
routes each through the same key its row does. **70** is the Mark history: that
an undo takes the *last-placed* mark whichever list it is in, that a redo puts it
back with the same coordinates (an arrow whose endpoint was lost would be a
different arrow), that a new placement discards the redo branch, that Reset
clears the marks and the history with them, that the three rows grey out with
what there is to do and each does what it says, that the menu carries the two
kinds, a separator and the three actions in that order, and that `Ctrl+Z`,
`Ctrl+Y`, `Ctrl+Shift+Z` and `Ctrl+R` all route. Mutation-checked six ways — a
redo stack that never fills, a redo that loses an endpoint, a Reset that leaves
the order behind, a placement that keeps the old branch, a row that is always
enabled, and `Ctrl+Z` wired to redo — all caught. The alarm's threshold
field is pinned where it is defined rather than through the window: `a` must arm
at the field's value (assertion 25), the field's clamp and the vendor's rounding
are `view_model_test`'s, the height ranges are `surface_test`'s, and the fixed
window a fresh session starts with is `session_test`'s.

37b is the still writer's 2× case: `save_still()` sizes its buffer from the
snapshot's factor, so the assertion reads the written PNG's own IHDR back and
requires 512×384 — and, without a model, requires the native 256×192 still
instead of a failure. It is what would catch the buffer-size bug the 2× work
introduced, which would otherwise make every still silently fail.

The capture and the gallery were checked end to end through the real window as
well: a fixture run driven with `s`/`v`/`v`/`q` wrote a `.dyt.jpg` (219 KB), a
`.png` (70 KB, `file` says `PNG image data, 256 x 192, 8-bit/color RGB`) and an
`.mp4` (31 KB, `ffprobe` says `h264 256x192 25/1 50 frames`, 2.0 s), and the
port's own `dyt_dyt_read()` parsed the container back (rc 0, blob 1656 B, 196608
raw bytes, geometry 256×192 total 384 flags 0x1). A second run over a directory
of three saved items, driven with `g`/`o`/`x`/`g`/`q`, drew the list (the
screenshots show `gallery 1/3` with the highlight on the newest), opened the
still (line 3 read `viewing dyt_20260925-192004.dyt.jpg`) and exported a fresh
70 KB `256x192` PNG.

Clip playback was checked the same way, through the real window under the
offscreen platform (assertion 59) and a saved window grab: opening a clip put it
on the canvas and the strip's middle line carried the amber `playing
dyt_…-001916.mp4 2/8` badge, with line 2 elided to stop before it. The folder
header showed the directory the list was scanned from, and the Pictures default
is pinned in `view_model_test` rather than by eye.

What these cannot pin is the write's real return code, the worker's `SetParam`
branch, and the read-back itself, because all three need a live `dyt_capture_t`.
Those were verified against the camera instead. Arming emissivity with `e` and
confirming with `y` printed `dytqt: set emissivity = 1.00 -> rc 0`; quitting then
printed `dytqt: verify emissivity = 1.00 -> confirmed (raw 128)` from the
teardown read-back, and the same line appeared on the `r` reconnect path; a
separate `probe --read --param 3` read the slot as `128  1.0000` where it had
been `127  0.9922`. The original value was written back afterwards, and confirmed
by read-back.

## Packaging

```
make install                          # PREFIX=/usr/local by default
make install DESTDIR=/tmp/stage PREFIX=/usr
make uninstall
make rpm                              # build/dytqt-<version>-<release>.<arch>.rpm
make deb                              # build/dytqt_<version>_<arch>.deb
```

`install` lays out the freedesktop locations under `DESTDIR`/`PREFIX`:

| path | what |
|---|---|
| `$(BINDIR)/dytqt` | the binary |
| `$(DATADIR)/applications/dytqt.desktop` | the desktop entry |
| `$(DATADIR)/icons/hicolor/<N>x<N>/apps/dytqt.png` | 16, 24, 32, 48, 64, 128, 256 |
| `$(DATADIR)/dytqt/palettes/*.dat` | the 28 vendor palettes |
| `$(DATADIR)/dytqt/models/zoom2.mnn` | the super-resolution model (13 KB) |
| `$(LIBDIR)/dytqt/libMNN.so` | the MNN runtime — only when the build has one |

The palettes are not optional decoration: the engine falls back to **six**
built-in ramps when it cannot find them, so a package that shipped none would
silently offer 6 of 28 — which is exactly what happened before they were added
here. `dyt_vm_find_data_dir()` searches `<datadir>/dytqt/<leaf>` under each
`$XDG_DATA_DIRS` entry (default `/usr/local/share:/usr/share`), the location
both `/usr` and `/usr/local` installs use, so an installed binary finds them
from any cwd. Running from the source tree still prefers the tree's own
`palettes/`, which the search tries first. The model is found by the same search
(`dyt_vm_find_model`), which is why it goes under `dytqt/models` and not beside
the binary; it ships even in a build without a runtime, where it is inert.

`libMNN.so` is the one file with a layout constraint: the binary's rpath is
`$ORIGIN:$ORIGIN/../lib/dytqt:$ORIGIN/../lib64/dytqt`, so the library must land
exactly one directory up from `$(BINDIR)`. The three entries are one per layout,
because the two packagers disagree about which libdir that is:

| entry | layout |
|---|---|
| `$ORIGIN` | the build tree, where the Makefile satisfies it with a `build/libMNN.so` symlink |
| `$ORIGIN/../lib/dytqt` | `$(PREFIX)/lib` — `make install`'s default, and the deb |
| `$ORIGIN/../lib64/dytqt` | `$(PREFIX)/lib64` — Fedora's `%{_libdir}`, and the rpm |

`$ORIGIN` is resolved by the loader at run time, which is what lets the same
build work under `/usr` and `/usr/local` and from the build tree. Deliberately
*not* an absolute path: a package would ship that path (lintian's
`binary-or-shlib-defines-rpath`), and on a machine that happened to have it, the
app would load a library the package does not contain. `make install` skips the
library and says so when the build has no runtime.

The icon is rasterised from `packaging/dytqt.svg` at install time, at 8-bit
RGBA (the default was 16-bit, which some icon loaders handle poorly), so
`make install` needs ImageMagick (`magick` or `convert`).

Both packagers wrap that same layout, so neither file list can drift from
`install`: `rpm` stages through `make install` and hands the tree to `rpmbuild`,
`deb` stages through `make install` and hands it to `dpkg-deb`.

### RPM (native on this host)

`make rpm` needs `rpm-build` (`sudo dnf install rpm-build`). It stages into
`$(RPM_STAGE)` and builds with `rpmbuild -bb packaging/rpm/dytqt.spec`, whose
`%install` copies that stage into `%{buildroot}` — the spec compiles nothing, so
there is no `Source:`, no tarball and no `BuildRequires` (`rpmbuild` enforces
those, and `%build`/`%install` only copy files). Details worth knowing:

* **The dependencies are measured by rpm itself.** Its ELF generator reads the
  binary's `NEEDED` entries and resolves them against the host's provides, so
  there is no hand-written `Requires` to get wrong — unlike `DEB_DEPENDS`, which
  has to name Debian's soname packages by hand.
* **`libMNN.so` needs no special handling.** An unversioned SONAME is fine in a
  private directory: the generator emits `Provides: libMNN.so()(64bit)` for the
  shipped copy, which satisfies the `Requires: libMNN.so()(64bit)` it derives
  from the binary — inside the same package. Nothing is excluded or faked.
* **`License: GPL-3.0-only`** — `LICENSE` is the unmodified GPLv3 text, the deb
  copyright says "version 3", and no source header grants "or later".
* **The `%if %{with mnn}` block is the Makefile's `HAVE_MNN`.** `make rpm` passes
  `--with mnn`/`--without mnn` from the same variable that decides whether
  `make install` ships the library, so the two cannot disagree.
* **`%files` is an anti-drift guard.** Fedora sets
  `%_unpackaged_files_terminate_build`, so a file `make install` staged but the
  spec does not list fails the build rather than being silently dropped. The
  window chrome added nothing to it: the rail, the panel, the dialogs and the
  Help text are all built in the binary — the icons are `QPainter` vectors, not
  assets — so `make install` stages no new file and `%files` is unchanged.

`_topdir` and the stage default to `~/.cache/dytqt/{rpmbuild,stage}` — absolute
and space-free by design, because this tree's path contains a space. A
*relative* `_topdir` is worse than useless: `%install`'s preamble `cd`s to
`%{builddir}` first, so a relative `%{buildroot}` would resolve against
`%{_topdir}/BUILD` while rpm's own file lookups used the invocation directory.
The `rpm` recipe refuses a spaced value; `RPM_STAGE` and `RPM_TOPDIR` override
the defaults.

### deb (cross-format on this host)

`deb` wraps the same layout with `dpkg-deb`. Three details are worth knowing:

* **`Depends` is measured when it can be, and says so when it cannot.** With
  `dpkg-shlibdeps` (from `dpkg-dev`) present it is read off the binary's
  `NEEDED` entries; otherwise the recipe falls back to `DEB_DEPENDS` and prints
  which path it took, so a guess is never mistaken for a measurement. The
  default names Debian bookworm's OpenCV soname packages — override
  `DEB_DEPENDS` for another release.
* **`libMNN.so` needs no `Depends` entry** — it ships inside this package, and
  its own `NEEDED` entries (`libstdc++`, `libm`, `libgcc_s`, `libc`) are already
  covered. `dpkg-shlibdeps` cannot know that, so the measurement runs with
  `--ignore-missing-info` and omits it; the alternative, a fabricated `shlibs`
  file, would claim a library no Debian package provides. This host has no
  `dpkg-shlibdeps`, so the package built here took the `DEB_DEPENDS` fallback —
  the flag is what the measured path *would* do, not something exercised here.
* **The staged binary is stripped** (3.4 MB → 334 KB). `make install` keeps its
  symbols, so a local install stays debuggable.

This build host is Fedora, so the **rpm is the native artifact and the `.deb`
is the cross-format one**: `dpkg-deb` builds and inspects it, but nothing here
installs it. Both were inspected here — each package contains the file list
above, each stripped binary still carries the three-entry `$ORIGIN` rpath, and
running each staged `usr/bin/dytqt` with `XDG_DATA_DIRS` pointed at the matching
staged share loads the staged model and passes the full selftest. That last one
is the end-to-end check that the two relative paths (the rpath and the data
search) are right.

Re-verified after the 3D Analysis tab landed (this host, Fedora 44 Asahi on
aarch64): `rpmbuild` resolved the new `libQt6OpenGL.so.6` and
`libQt6OpenGLWidgets.so.6` NEEDED entries into the package's `Requires` by
itself — no hand-written dependency, so the metadata cannot drift from what the
binary actually links — and the extracted `usr/bin/dytqt --selftest` reports
`backend GL` for the 3D tab and passes. The deb's fallback `Depends` names the
matching `libqt6opengl6`/`libqt6openglwidgets6`, and `packaging/check.sh` pins
that it does.

Both packages have been rebuilt and their extracted binaries run after **every
feature since** (the Comparison tab, the Circuit Design overlay, the serial and
the connection dot), with no packaging change needed for the last three: the
layout image is user-supplied and the serial and dot are the same binary. The
re-verification is `make deb && make rpm`, then `dpkg-deb -x` / `rpm2cpio` into
a scratch tree and run its `usr/bin/dytqt --selftest` **from the project
directory**, because the default fixture path is relative to the cwd.

The RPM was then installed for real — `sudo dnf install ./build/dytqt-*.rpm` —
and the **GUI launched from `/usr/bin/dytqt`**, which is exactly what the
staged-tree `--selftest` could not catch: the default source was the
source-tree fixture, so a menu launch (cwd `$HOME`) died on a missing
`testdata/…raw` before a window appeared. The default now falls back to the
camera when no fixture is readable — see "Run" — and launching the installed
binary from both a checkout and `/tmp` is the check that covers it. That
installed build **predates the redesign**, so re-running the install is the
last step of this work and is not done here: it changes the host.

`make check` runs `packaging/check.sh`, which pins the cross-file invariants a
syntax linter cannot see: that the entry's `Exec`, `Icon` and `StartupWMClass`
name the binary the Makefile installs, the icon it ships, and the WM_CLASS the
app actually sets — `("dytqt","dytqt")`, measured with `xprop`, not assumed —
that the entry's `Name` and the window title are one string (they share the
`kAppName` constant, so a rename that touched only one would show up as a menu
entry that opens a window titled something else),
that the icon rasterises to something non-blank at 16 and 256 px, that the model
ships where the app's own search looks, that a binary linking `libMNN.so` carries
an rpath covering both packaged layouts and no path into the build tree, and that
the spec agrees with the Makefile about the library's directory, the MNN switch
and the version. Each of those fails *silently* otherwise: a typo in any of the
metadata entries validates cleanly and still launches nothing, a model in the
wrong place looks exactly like a build without a runtime, a bad rpath makes the
installed app not start at all, and a spec that disagrees with `make install`
builds a package that cannot find its own library.

The version is single-sourced: the Makefile's `VERSION` feeds `-DDYT_VERSION`
for the GUI, names the deb, and is passed to the spec as `_dytqt_version`, so the
About box and both packages cannot disagree.

## Where the frames come from, and on which thread

Both the window and `--selftest` replay a frozen fixture through
`tools/frame_source.c`, which runs the real device-free pipeline
(`dyt_pipeline_resolve/frame`, `dyt_visible_extract`, `dyt_session_process`)
exactly as the live capture adapter does. The fixture also hands the session its
own copy of the payload (`dyt_session_process_raw`), the same call the adapter
makes — a still is written from the GUI thread long after the frame callback has
returned, so the session must own the raw, and keeping the fixture faithful is
what lets `--selftest` exercise the still writer and the recorder with no
camera. So the app needs no camera to be built, run or tested.

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
into. Otherwise a final teardown job runs and the exit waits up to 3 s for it —
2.25 s of it being the slot read-back's budget for the device to answer, paid
only when the session wrote a parameter — then abandons it the same way.

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
* **The runtime write is only exercised on hardware.** `dyt_capture_set_param()`
  is the one device write the port makes, and `--selftest` intercepts the send
  rather than issuing it, so the worker's `SetParam` branch, the real return code
  and the deferred read-back are all camera-only (see "Runtime parameters and the
  device panel"). The claim that a control transfer does not disturb the
  isochronous stream rests on the vendor doing the same thing, not on a
  measurement of this port.
* **A write cannot be confirmed while it is being used.** The read-back needs the
  stream stopped, so the verdict arrives at the teardown — which means a write
  followed by a clean exit with no reconnect is confirmed only on stderr, and a
  write the user never follows with `r` shows `*` in the panel for the rest of
  the session. That is a device limitation, not a choice: see "Verifying the
  write" for the measurements behind it.
* **Why the device defers an order for ~6.7 s is not known.** The boundary is
  measured and sharp — with the write at 1 s, a stream stopped at 6.5 s reads
  `127` and one stopped at 6.75 s reads `128` — but nothing in the port fires
  there, and the two OUT transfers return `rc 0` on both sides of it, so the gate
  is inside the device. Whether it is a fixed delay, a warm-up, or an internal
  calibration is not established; only the ~6.7 s figure and its consequence
  (a stream that stops sooner discards the pending order) are.
* **The ladder has no rung for the device's own defaults.** Emissivity is
  `127/128` on the reference unit and the ladder steps `1.00, 0.95, …`, so a
  write cannot restore the stored value through the UI. The live check put
  `127/128` back with a throwaway helper rather than with the app.
* **A message can outlive its moment.** The transient write-result line is
  driven by the pump's tick counter, so a stream that stalls while a result is
  showing leaves it up until frames resume. Minor, and the reference viewer's
  poll-driven TTL behaves the same way.
* **The super-resolution model is pinned, not judged.** `mnn_test` compares its
  output against the vendor's own `mnn_run_2` to within one LSB, so the
  arithmetic is verified — but whether 2× of a 256×192 plane *looks* better is
  not measured here. Mode `Z` has no vendor counterpart at all: it is the same
  model and the same arithmetic applied to a plane the vendor never fed it, and
  that (not a quality claim) is the whole of what the port establishes about it.
  Nor has SR been run against the camera: the live dual-half frame splits to
  exactly 256×192, so it is expected to apply, but the verification here is the
  fixture's, and the rate the model adds to the live path is unmeasured.
* **A failed 2× allocation would report active while rendering plain.** The
  snapshot's `sr_active` is the *predicate* — the factor the next render will
  use — rather than "what the last render did", which is what lets a front end
  size its buffer for the frame it is about to be handed. The cost is that if
  the session cannot allocate the 2× buffers it renders plain for that frame
  while the status line still says `x2`. It takes a failed `realloc` on a
  512×384 frame to reach, and is not reachable from a test.
* **Playback is tick-paced, not time-based, and cannot seek.** A clip advances
  one frame per pump tick, so it plays at the window's `--fps` regardless of the
  rate it was recorded at — a 25 fps clip is right at the default, and a clip
  recorded at another rate plays at the wrong speed. There is no seeking, no
  loop counter beyond the reader's own wrap, and no audio (the clips carry
  none). The decoder is OpenCV's ffmpeg backend, so it reads what that reads.
* **A still or a playing clip has no colour bar and no measurement.** The
  override draws the image and names it, but the bar and the tool overlays
  belong to the live session's snapshot and range, which a still's render does
  not share. Measuring a saved still is a later task.
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
  Driving the window with XTEST needs `QT_QPA_PLATFORM=xcb`: under the Wayland
  platform plugin the window is a native Wayland surface that X11 cannot see, and
  the key sender reports "no window matching" for a window that is plainly there.
* **The circuit-layout overlay is stretched, and placed by hand.** The layout
  image is scaled to the drawn picture with the aspect ratio ignored, so a
  layout whose own aspect does not match the sensor's (4:3) is distorted rather
  than letterboxed — the choice is deliberate (a layout is meant to cover the
  picture, and the align offsets are how it is fitted), but it is not
  registration. There is no auto-alignment and no homography: the user lines it
  up with the X/Y offsets, which shift in drawn-image pixels and are clamped to
  ±40. A layout drawn at the sensor's size needs no scaling and is exact.
* **The live-only states are asserted, not observed.** The connection dot's
  green/amber/red and the serial segment are exercised through `--selftest` with
  a synthetic `dyt_device_info_t` and each `DevState` in turn, because no camera
  is attached on this host. What the fixture path actually shows — the grey dot
  and no serial — is the one case a render here can confirm. A real camera's
  serial, and the dot's transition across a real connect/stall/reconnect, are
  camera-only.
* **The 3D surface's GL path is exercised headlessly.** `--selftest` runs under
  the offscreen plugin, which on this host does give a valid GL context, so
  `initializeGL`, the shader build and the buffer upload all run — but against
  the offscreen surface, not a compositor's. The software renderer is what the
  coverage assertion pins, and it is the path that exists everywhere. The same
  applies to the layout overlay, which is pure `QPainter`.
* **The `.deb` is not installed by its own package manager here.** The RPM *is*:
  `sudo dnf install` completed on 2026-09-25 and laid the files out under `/usr`,
  which is what surfaced the fixture-default bug above. That installed build
  **predates the redesign** (the rail, the tabbed panel and every feature from
  it), so `/usr/bin/dytqt` is not what `build/dytqt` is; re-running the install
  is the last step of the redesign and needs an explicit go-ahead, because it
  changes the host. The deb is only built, inspected with `dpkg-deb` and run from
  its staged tree — `dpkg -i` is not run, and its dependency closure is checked
  by resolving every `Requires` against the host's package database rather than
  by installing.
* **The RPM is a local artifact, not a Fedora submission.** It builds with this
  host's `rpmbuild` (6.0.2) and has not been through a Fedora review, `fedpkg` or
  `mock`. It ships no AppStream metainfo — `rpmlint` would say
  `no-appstream-metadata` — and no `Provides: bundled(...)` for the statically
  linked libuvc/stb or the shipped MNN, both of which a review would require. Its
  `Requires` are what *this* host's rpm resolved the binary's `NEEDED` entries
  to; its OpenCV soname is `.413`, so another Fedora release could resolve to a
  different set.
