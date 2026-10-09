# Measuring temperature

This page covers the measuring tools, the hot/cold markers, the chart, the alarm
and the isotherm. All of them live in the **Temperature Measurement** and
**High Temperature** groups of the Troubleshoot tab, and all of them have a key.

## The measuring tools

There are four tools plus a way to clear them. Pick one, then draw on the picture.

| Key | Tool | What it measures |
|---|---|---|
| **`p`** | Point | The temperature of a single pixel |
| **`l`** | Line | The temperatures along a line you draw |
| **`b`** | Box (rectangle) | The statistics of a rectangular area |
| **`o`** | Polygon | The statistics of a five-sided shape you drag out |
| **`n`** | None | Puts the tool away and clears what is placed |

The same choices are the **Temperature Measurement** buttons on the panel.

### Drawing with the mouse

One simple gesture covers every tool:

- **A click** (press and release without moving) places a **point** — because both
  ends of the "line" land on the same pixel.
- **A press, drag and release** draws a **line**, a **box**, or a **polygon**,
  depending on the tool you picked.

For the **box** and the **polygon**, once the shape is drawn you can edit it:

- **Drag its body** to move the whole shape.
- **Drag any of its eight handles** (four corners and four edge midpoints) to
  resize it. Dragging one handle changes only the edges that handle controls, so a
  right-edge drag never quietly changes the shape's height.
- **Press outside** the shape to start a fresh one.

The polygon is fitted to the box you drag out, so it stretches to touch all four
sides of that box. Move the box and the polygon follows.

### Reading the result

The **second status line** reports the current measurement, for example:

```
box (10,10)-(60,50) n=2091
```

For an area tool the label carries the statistics — **min / max / avg / med**
(smallest, largest, average and middle temperature in the shape). If a statistic
cannot be computed it reads `--` rather than a misleading `0 C`.

## The hottest and coldest pixels

The app marks the frame's **hottest pixel with `H`** (in red) and its **coldest
with `L`** (in blue). Press **`m`** to show or hide these markers. This is the
**Tracking** row in the High Temperature group.

## The temperature under the pointer

As you move the mouse over the picture, a small crosshair follows it and the
temperature of the pixel underneath is shown. This is the quickest way to probe a
spot without placing a tool.

## The chart (line profile)

The **Analysis** group has two rows that are two views of the same thing:

| Row | Key | What it shows |
|---|---|---|
| **Line** | `l` | Draws the temperature *along a line* as a graph |
| **Chart analysis** | `c` | The same line, with the mean and median marked and the peak called out with its value |

Choosing either puts the line tool up, so you can draw the line the chart will
plot. The chart is drawn below the picture: the horizontal axis is position along
the line, the vertical axis is temperature. Where the line leaves the image, the
graph shows a gap rather than inventing a value.

## The alarm

Press **`a`** to arm the temperature alarm, and **`a`** again to disarm it. When
the alarm trips, a badge appears on the first status line.

The alarm uses a **band** of temperatures derived from the current range — the
middle 40 % of it, with a little hysteresis so it does not flicker. You can set
the threshold explicitly with the **High TEMP. Alarm** field in the High
Temperature group (it accepts roughly **−20 °C to 450 °C**); the alarm arms at
whatever value that field holds.

> Two honest caveats, inherited from the vendor's own behaviour: arming the alarm
> fixes its band **at the moment you press the key**, so if the camera's automatic
> range later settles somewhere else, the alarm band no longer matches the scene.
> And pressing `i` on its own (below) dims the whole image, because with no alarm
> armed the "highlighted band" is empty.

## The isotherm

Press **`i`** to toggle the **isotherm**. The isotherm highlights the pixels
inside the alarm's temperature band and dims everything else, so the parts of the
scene at the temperature you care about stand out. This is the **Highlight** row
in the High Temperature group.

---

**Next:** [Notes and arrows (Mark) →](Annotations.md)

*Back to [Home](Home.md)*
