# models — the vendor's super-resolution model, recovered

| file | bytes | sha256 | what it is |
|---|---|---|---|
| `zoom2.mnn` | 12,928 | `6a86cf014f937cb9933a03af8d1eacefdd444a60a1d9a3b0ad24dded9f6a6ae4` | the app's 2× zoom (super-resolution) model, as an MNN flatbuffer |

## Where it came from

The Android app ships an MNN inference runtime and a JNI shim, but **no model
file** — `assets/` holds only the seven palette `.dat` files and the PDFs, and
`res/raw` is empty. The weights are embedded, obfuscated, inside
`libmnnmodel.so` itself. **[V]** (2026-09-25)

The path that reaches them, from the arm64-v8a disassembly:

```
AbstractUVCCameraHandler.CameraThread.<init>
    MImageUtils.initMNNModelModule(activity)     -> 40-byte no-op; sets a flag,
                                                     ignores the Context
    MImageUtils.MRun1(MNN_ZOOM_X2, 2)
        native_mnn_run_1 -> mnn_run_1(1, 2) -> sr1(level, key)   @ 0x6010
            buf = operator new[](size)
            buf[i] = .data[i] ^ key[i % 25]          # 25-byte repeating XOR
            MNN::Interpreter::createFromBuffer(buf, size)
    ...per frame...
    MImageUtils.MRun2(MNN_ZOOM_X2, in, out)      # out is byte[393216]
    MImageUtils.MRun3(MNN_ZOOM_X2)               # teardown
```

So the ciphertext is the whole of `.data` from VA `0xf120`, its length is a
`uint32` global at VA `0x123a0`, and the key is 25 bytes at `.rodata` VA
`0x22e0`. `RE Workspace/tools/extract_mnn_model.py` re-derives the file from
`libmnnmodel.so` and asserts each of those addresses falls inside the section it
belongs to, so a changed library fails loudly rather than yielding a
plausible-looking wrong file.

Regenerate with:

```
cd "RE Workspace/tools"
python3 extract_mnn_model.py ../apk/lib/arm64-v8a/libmnnmodel.so ../../linux-port/models
```

## What the model is

An ESPCN-style super-resolution network, exported from ONNX — the flatbuffer's
op names are the ONNX node names:

```
image_input
  onnx::Conv_14, Conv_16, Conv_18, Conv_20, Conv_22      # 5 convolutions
  onnx::Add_23, Add_25                                    # a residual add
  onnx::DepthToSpace_26                                   # the 2x upsampler
  onnx::Clip_27
image_output
```

`DepthToSpace` is the pixel-shuffle upsampler, which is what makes this a 2×
zoom rather than a resample. The flatbuffer's asset UUID is
`7006ec85-d318-4f47-9508-fbbe5f08ec82`.

## The shape contract

Three independent sources agree, and one of them is the model's own reshape call:

* **The reshape.** `sr1` builds a dims vector and calls
  `MNN::Interpreter::resizeTensor(input, dims)` with it. The instructions are
  unambiguous — `mov w8,#1; str; str; mov w8,#0xc0; str; mov w8,#0x100; str`,
  i.e. **`[1, 1, 192, 256]`**: NCHW, batch 1, **one channel**, height 192,
  width 256. So the model takes a single 256×192 plane, and the input geometry
  is *not* baked into the flatbuffer — the app sets it at load time.
* **The call site.** `AbstractUVCCameraHandler` allocates the output as
  `new byte[393216]`. 393216 = 512 × 384 × 2, i.e. **512×384 at two bytes per
  sample**.
* **The graph.** A single `DepthToSpace` with block size 2 maps a `(C·4, H, W)`
  tensor to `(C, 2H, 2W)`. With C = 1 in and C = 1 out — the last `Conv` emits
  4 channels — a 256×192 input gives exactly 512×384. That is the 2× upscale,
  and it is consistent with both numbers above.

| | width | height | samples | bytes (2 B/sample) |
|---|---|---|---|---|
| input | 256 | 192 | 49,152 | 98,304 |
| output | 512 | 384 | 196,608 | **393,216** |

`src/mnn.c` states this contract as data and `src/mnn_test.c` asserts it, so a
change to either side is caught without a model runtime.

## Running it

The model is executed by `src/mnn_runtime.cpp`, behind the `dyt_mnn_zoom2()`
seam in `src/mnn.c`. The seam is C and runtime-free; everything needing MNN's
C++ API lives in the `.cpp`, which the Makefile compiles only when an MNN
runtime is installed (`HAVE_MNN`). Without one, `dyt_mnn_zoom2()` reports
`DYT_MNN_UNAVAILABLE` and refuses — see the recipe in
`third_party/README.md`.

The conversion is not guessed; it was read out of the vendor's own per-frame
code (`sr2` in `libmnnmodel.so`) and then confirmed against it:

```
input   ldrb  w8, [in + i*2]        # the LOW byte of each uint16 sample
        dst[i] = (float)(w8 / 255.0)   # divisor is the double at .rodata 0x2d80

output  s0 = out_f[i] + 32768.0f    # fadd 0x47000000
        out[i] = (uint16_t)trunc(s0)   # fcvtzu
```

The network has unit DC gain, so the 32768.0 is a pure offset: feeding
`mnn_run_2` constant planes and reading the uint16 back gives `out - 32768 == in`
to within a rounding step.

### The layout trap

The session's input tensor is **NC4HW4** even though it reports
`getDimensionType() == CAFFE`. For the 1-channel 192×256 input it reports
`elementSize() == 196608` and `dim[0].stride == 196608` — four times the 49152
the shape implies, because the channel dimension is padded to 4. Writing
samples straight into `host<float>()[i]` therefore does **not** fill pixels
0..49151; it fills pixels 0..12287 across all four channels, and the frame comes
out garbage. Measured against the vendor on the same plane, that naive write
gets 177611 of 196608 samples wrong with a peak error of 253, while going
through an explicit plain-NCHW host tensor and `copyFromHostTensor()` /
`copyToHostTensor()` matches the vendor to within one LSB.

A *constant* test plane cannot detect this — a constant fill looks identical in
every layout. That is why the differential uses a non-constant plane.

### The differential

`tools/mnn_diff/` drives the vendor's own `mnn_run_2` out of `libmnnmodel.so`
on this host and freezes its answer for a fixed input plane in
`tools/mnn_diff/out/`. `make check` upscales that same plane through
`dyt_mnn_zoom2()` and compares:

```
177081/196608 bit-exact, 19527 differ, max|delta| 1
means: input 106.77, ours 32874.29, vendor 32874.38
```

The residual is not a convention error — those show up as a peak error of 255.
It is deterministic floating-point drift between the vendor's MNN 2.5.0 and the
upstream MNN this port builds against; it is unchanged by thread count
(`numThread` 1/2/4/8 are bit-identical) and by `BackendConfig::Precision`
(`Normal` and `High` agree exactly). See `tools/mnn_diff/out/meta.txt`.

## What is *not* established

* **Which plane it zooms is `[I]`.** The Java method that feeds it is
  `onYUVtoJava`, which suggests the *visible* plane rather than the thermal one,
  but the buffer sizes alone do not prove it. Treat the input as "a 256×192
  8-bit-ish plane" until that is pinned. The differential above pins the
  *arithmetic*, not which plane the app hands it.
* **Edge behaviour is only as good as the vendor's.** The comparison above is
  against the vendor's own output, so it says this port reproduces the vendor —
  not that the vendor's model is itself a good upsampler.
* The 25-byte key is an obfuscation measure, not cryptography; it is recorded in
  the extraction script only because it is needed to reproduce the file.
