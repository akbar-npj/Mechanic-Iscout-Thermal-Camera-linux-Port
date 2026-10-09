# Comparing two boards (Comparison tab)

The **Comparison** tab takes a **saved reference** — a still of a known-good
board — and compares it, pixel by pixel, against the live picture. It is the
fastest way to answer "what is different about this board?" when you have a
healthy one to hand.

## Using it

1. Put the reference board under the camera and **save a still** (press `s` — see
   [Saving and browsing](Capture-and-gallery.md)). This is your "good" board.
2. Put the board you want to test under the camera.
3. Open the **Comparison** tab (or click the **Compare** icon on the rail) and
   click **Load reference…**, then choose the still you saved.
4. The tab shows the difference between the reference and the live frame.

If you prefer, you can load a reference at start-up from the command line with
`dytqt --reference PATH`.

## What it shows

The comparison produces:

- A **signed difference picture** — each pixel is the reference minus the live
  temperature, so a pixel that is *hotter* than the reference shows up one way and
  a *cooler* one the other.
- **Per-pixel statistics** — a summary of how far the two differ.
- A **50/50 blend preview** — the reference and the live frame overlaid, so you
  can see where they align.

A **threshold** box lets you set how big a difference counts as "beyond the
threshold"; the stats report how many pixels are past it. Setting it low catches
small differences, setting it high ignores noise and shows only the dramatic ones.

Click **Clear reference** to drop it and go back to comparing nothing.

## A note on sizes

The reference and the live frame must be the same size to be compared. If the
reference was saved at a different resolution (for example, a different sensor
width), the stats box stays **empty** rather than showing a comparison that does
not line up — an empty box is the honest answer, not a broken one.

---

**Next:** [Circuit layout overlay →](Circuit-Design.md)

*Back to [Home](Home.md)*
