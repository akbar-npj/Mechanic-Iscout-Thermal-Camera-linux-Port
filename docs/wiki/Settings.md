# Settings

The **Settings** dialog opens from the **Setting** icon at the bottom of the left
rail. It gathers the options that are not worth a permanent button into three
groups.

The dialog is **non-modal**: it stays open beside the picture while you work, so
you can change a setting and watch the reading move. It re-fills itself each time
you open it, so it always shows the camera's current values.

## Group 1 — the runtime parameters

The first group shows the four camera settings — **emissivity, ambient, reflected
and distance** — as **number fields** you can type into. Each row has its own
**Send** button.

This is the same change described on [The camera itself](The-device.md#the-four-stored-settings),
just with a text box instead of the key ladder. One **Send per row** rather than a
single button for the whole group, because each setting is armed and confirmed on
its own.

The number fields are bounded by the same range the keyboard ladder can reach, so
the dialog can offer more convenience but never a value the keyboard could not.

A setting the camera has never reported is labelled **"(not read)"** and starts at
the ladder's first value, rather than showing a plausible-looking zero. A value you
changed this session takes precedence; a **failed** write does not.

## Group 2 — Display

The Display group holds the view options:

| Control | What it does |
|---|---|
| **Unit** | Chooses Celsius, Fahrenheit or Kelvin (same as the `u` key) |
| **Zoom** | Sets the magnification (same as `+` / `-`) |
| **Full screen** | Turns full screen on or off (same as **F11**) |
| **Device panel** | Shows or hides the device panel (same as `d`) |
| **Retry** | Retries the camera connection immediately (same as `r`) |
| **About…** | Opens the About box (same as `?` and `F1`) |

Everything here is re-seeded each time you open the dialog, so a change made from
the keyboard cannot leave the dialog showing the old value.

> Palette and Fusion are deliberately **not** here — they have their own choosers
> on the rail, and two routes to one list would be one too many.

## Group 3 — Toolbar

The **Toolbar** group decides which items the **rail** and the **panel tabs** show.
Each rail icon and each panel tab has a tick box; untick one and it disappears from
the window.

This is for trimming the interface to how you work. If you never use, say, the
Comparison tab, you do not have to give it room in the tab bar.

Two things to know:

- **Setting is never hidden.** It is where this chooser lives, so a user who could
  hide it would have no way back. Its tick is checked and disabled, and the app
  refuses a request to hide it even if one is made directly.
- Your choices are **remembered** the next time you open the app.

---

**Next:** [Keyboard shortcuts →](Keyboard-shortcuts.md)

*Back to [Home](Home.md)*
