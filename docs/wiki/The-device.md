# The camera itself

This page covers the **device panel**, the camera's four **stored settings** you
can change, and what happens when the camera disconnects.

## The device panel

Press **`d`** to show or hide the **device panel**. It appears in the top-left
corner over the picture and lists:

- the camera's **module serial number**,
- the **decoded user serial number**,
- the four **stored settings** (below), and
- the **slot count**.

It is a quick way to confirm exactly which camera is attached and what it is
currently set to. (The camera's serial also appears on the status strip's third
line, next to the connection dot.)

## The four stored settings

The camera holds four radiometric settings that affect its temperature readings.
These are the **only** things the app ever *writes* back to the camera:

| Key | Setting | What it means |
|---|---|---|
| **`e`** | **Emissivity** | How strongly the surface radiates heat. Shiny metal radiates less than matte surfaces, so its true temperature needs a different value. |
| **`A`** | **Ambient** | The surrounding air temperature. |
| **`R`** | **Reflected** | The temperature of heat reflected off the surface from nearby objects. |
| **`D`** | **Distance** | How far the camera is from the target. |

### The keys are case-sensitive

This matters, and it is the whole difficulty of these controls:

- **`e`** (lowercase) is emissivity, but **`A`**, **`R`** and **`D`** are the
  *capital* letters.
- The lowercase letters are already taken: **`a`** is the alarm, **`d`** is the
  device panel, and **`r`** is retry.

So: **`e`** emissivity, **`A`** ambient, **`R`** reflected, **`D`** distance.

### Arming and sending

A setting is changed in **two steps**, on purpose. The camera applies these
immediately, so a stray keypress must never change the reading:

1. Press the setting's key to **arm** it. This does not send anything yet — it
   just picks the setting and shows a confirmation on screen. Pressing the key
   again walks through a ladder of suggested values.
2. Press **`y`** to **send** the armed value to the camera.

While a value is armed, the app takes the keyboard: every other key is swallowed
until you confirm or cancel, so a pending write cannot be disturbed. Press
**`n`** or **`Esc`** to cancel. The one key that always works is **`q`** (quit),
which is never swallowed.

You can also set the values numerically in the [Settings](Settings.md) dialog,
which sends through the very same path.

### The `*` mark

After you write a value, the device panel shows it with a **`*`** suffix and the
slot line adds `(* = set this session)`. That means *written, not yet confirmed* —
the confirmation can only arrive later, as explained below.

## Confirming a write

The app checks a write by reading the setting back from the camera. It cannot do
that while the camera is streaming, because the video stream starves the read. So
the **read-back happens when the stream stops** — either when you quit, or when you
press **`r`** to reconnect.

The result is printed to the terminal and, on a reconnect, shown on the status
strip:

- `write confirmed by read-back` — the camera took the new value;
- `write NOT confirmed` — it did not.

Because of this timing, a value written and then followed by a clean exit is
confirmed only in the terminal output. That is a limitation of the camera, not a
choice: see [Troubleshooting](Troubleshooting.md#things-that-are-not-yet-proven).

## Reconnecting

If the camera disconnects, or a stream freezes, the app **reconnects on its own**.
It waits a little longer each time it fails (half a second, then one, two, four,
eight, sixteen, thirty seconds), and gives up after a while and says so.

Press **`r`** to retry **immediately** without waiting for the next automatic
attempt. Reconnecting clears the device panel and any value you had armed, because
the freshly opened camera's own stored settings are the authority again.

The coloured dot on the status strip (see
[Getting started](Getting-started.md#the-status-strip-bottom)) tells you the
camera's state at a glance: green for live, amber while coming up or gone quiet,
red when no camera is found, grey for the saved-sample mode.

---

**Next:** [Settings →](Settings.md)

*Back to [Home](Home.md)*
