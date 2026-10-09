# The 3D view (3D Analysis tab)

The **3D Analysis** tab shows the heat as a **landscape**: temperature becomes
height. Hot spots rise up like hills and cold areas sink into valleys. It is a
fast way to see the *shape* of a heat problem — where heat is concentrated, how it
spreads, and how one board differs from another.

## Using it

- **Drag** on the surface to orbit around it.
- **Scroll the mouse wheel** to zoom in and out.
- Click **Reset view** to return to the opening camera angle.

The surface is coloured with your current **palette**, at the same temperature
range the 2D picture is using — so a hot peak is the same colour here as it is in
the flat view. The mesh is simplified a little so that it stays smooth to move
around, but the heights are the frame's real temperatures.

## The Height group

The Height group has two modes that decide what the *heights* mean. They are also
on the keys **`P`** and **`C`**.

| Mode | Key | What the height represents |
|---|---|---|
| **Morphological Change** | `P` | The temperature stretched across the **display window** (the range you are showing). Data outside that range flattens into a plateau. This is the default. |
| **Color Changes** | `C` | The temperature stretched across the **frame's own hottest and coldest values**, leaving the display window to control only the colours. |

The colour is taken the same way in both modes — from the display window — so
switching the height mode changes the *shape* of the landscape, not its colours.

> **Two things worth knowing.** The two modes look identical while the range is on
> **automatic**, because there the display window *is* the frame's extremes — there
> is nothing to distinguish. Set a fixed range (press `t`) and they separate
> clearly. And **Small Current Leakage** in the Circuit Mode group is the one mode
> that jumps you to this tab automatically, because the 3D view is the point of it.

## If it looks flat or plain

The 3D view prefers to draw with your graphics hardware, and falls back to a
software renderer when that is not available. Either way the tab works; if your
machine has no hardware acceleration the surface is simply drawn by the processor
and may be a little less smooth. Nothing is missing.

---

**Next:** [Comparing two boards →](Comparison.md)

*Back to [Home](Home.md)*
