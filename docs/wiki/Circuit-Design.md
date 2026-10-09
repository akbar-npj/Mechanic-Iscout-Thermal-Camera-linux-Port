# Circuit layout overlay (Circuit Design tab)

The **Circuit Design** tab lays a **picture of a circuit board** on top of the
thermal image. Instead of seeing a hot blob and wondering which component it is,
you see the hot blob *on the board drawing* and can name the part straight away.

## Using it

1. Open the **Circuit Design** tab.
2. Click **Load layout…** and choose a board layout image (a photo, a scan, or an
   exported CAD drawing). You can also load one at start-up with
   `dytqt --layout-image PATH`.
3. The layout appears over the thermal picture.

Loading an image turns the page's **show** toggle on automatically, so a load that
happened while the overlay was switched off never looks like it failed.

## Lining it up

The overlay has three controls:

| Control | What it does |
|---|---|
| **Show** | Turns the overlay on or off without forgetting the image |
| **Align X / Align Y** | Nudges the layout left/right and up/down to line it up |
| **Opacity** | Makes the layout more or less transparent, so you can see the heat through it |

The align offsets run from **−40 to +40** and are measured in the *drawn picture's*
pixels, not the sensor's — so an offset means the same visible shift no matter how
far you have zoomed in. That range is the camera's own alignment limit, the same
one the thermal/visible alignment uses.

**The layout turns with the picture.** On the live view, if you rotate or mirror
the thermal image, the overlay rotates and mirrors with it, so the board drawing
stays stuck to the board. (A saved still or clip already has its orientation baked
in, so its layout is drawn as loaded.)

## How it is drawn

The overlay is composited *under* everything else — under the colour bar, the hot
and cold markers and the device panel — so those stay readable on top of it.

> **Honest limitation:** the layout image is **stretched** to cover the picture,
> ignoring its own aspect ratio, and it is placed **by hand** with the X/Y offsets.
> There is no automatic alignment. A layout drawn at the sensor's own size needs no
> stretching and lines up exactly; one with a different shape is fitted with the
> offsets. See [Troubleshooting](Troubleshooting.md#things-that-are-not-yet-proven).

Click **Clear layout** to remove the overlay.

---

**Next:** [Super-resolution (2×) →](Super-Resolution.md)

*Back to [Home](Home.md)*
