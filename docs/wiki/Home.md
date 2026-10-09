# Mechanic iScout Thermal Camera — the layman's wiki

<p align="center">
  <img src="../images/mechanic-iscout-thermal-camera.jpg"
       alt="Mechanic iScout USB thermal imaging camera on its adjustable stand"
       width="380">
</p>

Welcome. This is a plain-language guide to the **Mechanic iScout Thermal Camera**
desktop app — the Linux program in this repository. It explains, in ordinary
words, what every button, menu and key does and when you would want it.

You do **not** need to know anything about programming, USB, or reverse
engineering to read this. If a word is technical, it is explained where it first
appears.

> **What is this thing?** A small thermal camera on a stand. It plugs into a
> computer over USB and shows a live heat picture of whatever is under it. It is
> most often used to find hot or shorted parts on a circuit board. This app is
> the Linux software that drives it — the manufacturer only made Windows and
> Android software.

## Start here

| Page | What it covers |
|---|---|
| [Getting started](Getting-started.md) | Installing, opening the app, and the parts of the window |
| [The picture](The-picture.md) | Colour palettes, zoom, rotate, mirror, fusion, the range and the colour bar |
| [Measuring temperature](Measuring-temperature.md) | Point / line / box / polygon tools, the hot and cold markers, the chart, the alarm and the isotherm |
| [Notes and arrows (Mark)](Annotations.md) | Drawing text labels and arrows on the picture, and undoing them |
| [The 3D view](3D-Analysis.md) | The 3D Analysis tab — a height map of the heat |
| [Comparing two boards](Comparison.md) | The Comparison tab — diff a known-good board against the live one |
| [Circuit layout overlay](Circuit-Design.md) | The Circuit Design tab — lay a board drawing over the thermal picture |
| [Super-resolution (2×)](Super-Resolution.md) | The Super Resolution tab — a sharper, enlarged picture |
| [Saving and browsing](Capture-and-gallery.md) | Stills, video clips, and the gallery |
| [The camera itself](The-device.md) | The device panel, its stored settings, and reconnecting |
| [Settings](Settings.md) | The Settings dialog — parameters, display options and which buttons to show |
| [Keyboard shortcuts](Keyboard-shortcuts.md) | The complete key list in one table |
| [Troubleshooting](Troubleshooting.md) | When something does not work, and what is not yet proven |

## The 30-second tour

1. **Plug the camera in** and open the app. A live heat picture appears.
2. **Drag on the picture** to measure a temperature — the reading shows below it.
3. **Everything the keys do is also on screen**: the icons down the left, and the
   panels down the right. You never have to memorise a key.
4. **Press `s` to save a picture**, `v` to record a clip, and `g` to browse what
   you saved.

## A word about honesty

The deep engineering notes in this repository tag every claim as **verified**,
**inferred**, or **unknown**. This wiki is the friendly layer on top, but it keeps
that spirit: where the software cannot yet do something, or has not been checked
against a laboratory thermometer, [Troubleshooting](Troubleshooting.md) says so
plainly rather than pretending.

---

*This wiki explains the **app**. For building it from source, see
[`BUILDING.md`](../../BUILDING.md); for the reverse-engineering detail, see
[`RE Docs/`](../../RE%20Docs/README.md).*
