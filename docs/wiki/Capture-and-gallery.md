# Saving and browsing

This page covers saving stills, recording clips, and the **gallery** for browsing
what you saved.

## Saving a still

Press **`s`** to save the current frame. It writes **two files** at once:

- a **`.dyt.jpg`** — a normal JPEG that also carries the raw temperature data
  inside it, so the picture can be re-rendered later; and
- a **`.png`** — the same picture for any ordinary image viewer.

The **Still** button in the Capture group does the same thing.

## Recording a clip

Press **`v`** to **start** recording a video clip, and **`v`** again to **stop**
and finalise it. The clip is written as an `.mp4`.

While a recording is running, the status strip shows a red **`REC`** badge with the
elapsed time and frame count, so it is always obvious that you are recording. The
badge stays put; the little messages that come and go do not disturb it.

The **Record** button in the Capture group toggles the same recording.

> Clips need the OpenCV library to be present. If the app was built without it, `v`
> says so plainly rather than failing silently.

## Where files go

Stills and clips are saved into a **capture folder**. By default this is your
**Pictures** folder. To change it:

- Click **Folder…** in the gallery header (see below), or
- Use the **Choose folder…** action, or
- Start the app with `dytqt --capture-dir FOLDER`.

The same folder is used for browsing and for saving, so what you saved is what the
gallery shows. Your choice is remembered the next time you open the app.

Files are named `dyt_<date>-<time>`, so two things saved in the same second never
collide.

## The gallery

Press **`g`** to show or hide the **gallery** — a list of the stills and clips in
your capture folder, newest first. It appears over the picture.

| Key | Action |
|---|---|
| `g` | Show or hide the list (hiding it also returns to the live view) |
| `↑` or `k` | Move the highlight up (wraps around at the top) |
| `↓` or `j` | Move the highlight down (wraps around at the bottom) |
| `Return` or `o` | Open the highlighted item |
| `x` | Export the highlighted still as a PNG |
| `space` | Pause or resume a playing clip |
| `Esc` | Close the list, or stop a playing clip, and return to the live view |

You can also use the **mouse**: click a row to select it, and double-click to open
it.

While the list is open it has the keyboard — a key it does not use is ignored
rather than reaching the measuring tools, so moving the highlight can never
accidentally change your tool.

### Opening a still

Opening a still **re-renders it from its saved temperature data** using your
current palette — you are seeing the real radiometric data, not just a JPEG. This
is what makes "same still, different palette" possible: export it with `x` and you
get a fresh PNG in the colours you are looking at now.

### Opening a clip

Opening a clip **plays it** on the canvas. `space` pauses and resumes without
losing your place; `Esc` (or closing the gallery) stops it and returns to the live
view. The status strip shows which clip and which frame you are on.

> A clip plays at the window's frame rate, not necessarily the rate it was recorded
> at, and there is no fast-forward or rewind. See
> [Troubleshooting](Troubleshooting.md#things-that-are-not-yet-proven).

## The disk guard

Recording needs free space. The app refuses to **start** a clip unless there is at
least **64 MB** free, and stops a running clip if free space falls below **16 MB**.
When it refuses, it tells you why — the folder is not writable, or there is only so
much space left — rather than a bare "recording failed".

---

**Next:** [The camera itself →](The-device.md)

*Back to [Home](Home.md)*
