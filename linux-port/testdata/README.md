# testdata — frozen raw frames for the no-hardware regression tests

`pipeline_test` drives the live capture pipeline (`dyt_pipeline_*` in
`frame.c`, the device-independent half of `capture.c`) over real frames.
Three fixtures are used:

| fixture | mode | role |
| --- | --- | --- |
| `mode1000_256x192.raw` | 1000 | real frame from the 0bda:5840 unit — the AD output mode |
| `mode1000_256x384_default.raw` | 1000 | the same unit's dual-half frame — the **default** output mode |
| `../tools/thermometry_diff/out/256/in_frame.bin` | 0x44c | the vendor ground-truth frame already in the tree |

## mode1000_256x192.raw

* 98,304 bytes — 256 × 192 × 2, i.e. 49,152 little-endian `uint16` samples,
  row-major, no reference band (mode 1000).
* Captured from the real device on 2026-09-24 with:

  ```
  capture_demo --start-orders --frames 600 --save-last --out /tmp/dyt_fixture
  ```

  `--save-last` matters: the device streams a flat `0x8000` filler until
  ~2-6 s after `setTinyCOutputADValue` lands, so the *first* frame is always
  the filler. The saved frame is frame 599.
* sha256 `2c6e8cbad624e5376a35358097a28bb02d33795895e7acdf24198f774f11e50e`
* Content: 9 distinct sample values, raw 19408..19472 → 30.10..31.10 °C
  (a near-uniform scene). It is deliberately *not* a detailed scene: the
  test asserts the conversion contract, not the picture.
* It is tracked because it is the only artifact that exercises the real
  per-unit capture path without hardware. Re-capturing it from a different
  scene will not change the test result — the assertions are
  `out[k] == raw[k]/64 - 273.15` over every sample, plus "not the filler".
* **Note the flag is now `--ad-output`**, not `--start-orders`: the AD order
  turned out to be a mode switch (see the other fixture), so the port does not
  send it by default.

## mode1000_256x384_default.raw

* 196,608 bytes — 256 × 384 × 2, i.e. 98,304 little-endian `uint16` samples,
  row-major. This is the device's **default** output frame: the port reads its
  bottom half as the thermal plane, with no vendor order at all.
* Captured from the real device on 2026-09-24 with:

  ```
  capture_demo --frames 400 --save-last --out /tmp/dyt384
  ```

  Again `--save-last`: the *bottom* half streams a flat `0x8000` filler for
  the first ~6 s, exactly as the AD-mode frame does. The saved frame is 399.
* sha256 `5d387878c18bd611b8dcde997d09ca0c6180fee8d829433b4446efaebcd7b920`
* Content, and why both halves are worth pinning:
  * **rows 0..191** — the device's grayscale visible image, not thermal. Every
    odd byte is `0x80`, i.e. YUYV with U = V = neutral, so it is a plain
    8-bit grey picture. Decoded as thermal it would read 239.69..240.24 °C,
    which is why the test asserts `max(out) < 100 C`: a regression that
    skipped the slice would silently report ~240 °C scenery.
  * **rows 192..383** — the real thermal plane, 16 distinct values,
    raw 19492..19556 → **31.41..32.41 °C**.
  * The two halves differ in every sample, so "did the slice happen" is a
    meaningful question rather than a coincidence.
* The thermal plane here is the *same data* as an AD-mode 256x192 frame
  (A-B-A interleave, 2026-09-24: cross-mode correlation 0.94 vs a 0.93
  same-mode noise floor), which is why one conversion covers both modes.
