# testdata — frozen raw frames for the no-hardware regression tests

`pipeline_test` drives the live capture pipeline (`dyt_pipeline_*` in
`frame.c`, the device-independent half of `capture.c`) over real frames.
Two fixtures are used:

| fixture | mode | role |
| --- | --- | --- |
| `mode1000_256x192.raw` | 1000 | real frame from the 0bda:5840 unit — the mode this port runs |
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
