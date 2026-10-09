# Super-resolution (2×)

**Super-resolution** enlarges the picture **2×** (from 256×192 to 512×384) using a
small trained model, so the image is bigger and a little sharper than simply
stretching the pixels. It is an addition this port recovered from the device's own
software, not one of the vendor's controls.

## The Super Resolution tab

The tab has three choices:

| Choice | Key | What it does |
|---|---|---|
| **Off** | — | No enlargement (the normal view) |
| **Visible plane (2×)** | `z` | Enlarges the **visible-light** half of the frame |
| **Thermal plane (2×)** | `Z` | Enlarges the **thermal** half |

`z` and `Z` are one feature with two planes, so they are tied together: turning one
on turns the other off. There is no sensible "both at once".

The **Off** row has no fixed key of its own, because `z` and `Z` each *toggle their
own plane* — pressing `z` while thermal is on would switch to visible rather than
clear. The row simply presses whichever key matches the mode currently in use.

## When it works, and when it does not

Super-resolution needs two things:

- **A model file** (`zoom2.mnn`) must be present. Without it the three choices are
  greyed out and the page says `No model loaded`. The app reports this honestly
  rather than offering buttons that do nothing.
- **A 256-pixel-wide sensor.** The model is fixed at 256×192 → 512×384, so a
  different sensor width is left alone rather than fed the wrong shape.

The **Visible plane** mode also needs a fusion pattern that actually *shows* the
visible half. With the default infrared-only pattern there is nothing on screen
for it to enlarge, so it reports itself as inactive.

When super-resolution cannot take effect, the status line says `sr:<name>
(inactive)` and a short message tells you the one thing to change. A key that
silently does nothing is the one thing this feature must not be.

## What happens to saved files

When super-resolution is on and you save a still, the **embedded picture** is the
2× image, while the file keeps the original (native) thermal data alongside it. So
a vendor tool can still re-render the still from its raw data, and you get the
sharper picture too — the best of both.

---

**Next:** [Saving and browsing →](Capture-and-gallery.md)

*Back to [Home](Home.md)*
