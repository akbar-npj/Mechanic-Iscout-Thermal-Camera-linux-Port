# RE Workspace — Mechanic iScout Thermal Camera

Generated analysis artifacts. **Disposable** — everything here is reproducible from
`../iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/` using the recipes in
`../RE Docs/02-toolchain-and-reproduction.md`.

Total size ~583 MB. Derived material, so `.gitignore` excludes it — with the exception
of the scripts under `tools/` and `ghidra/`, which are tracked because the docs cite
them as the reproduction pipeline.

```
apk/                    extracted from MechanicTi.apk
  lib/arm64-v8a/*.so       native libraries (analysis target)
  assets/*.dat             palettes (768 B each)
  assets/*.pdf             user manuals
  classes*.dex             Dalvik bytecode

jadx/                   jadx 1.5.6 decompilation (204 MB, 3493 .java files)
  sources/com/dyt/wcc/     the vendor application
  sources/com/serenegiant/usb/UVCCamera.java   JNI surface
  resources/AndroidManifest.xml
  resources/res/xml/device_filter.xml          USB VID/PID filter

exe/                    Windows installer analysis
  nsis_blocks/             blk000..blk206 extracted NSIS data blocks
    manifest.txt             block index -> offset/size/kind
    walk2_manifest.txt       same, from the exploratory walker
    block_file_map.txt       block -> identified file name/type
  boot1/ boot3/            extracted VC++ bootstrapper CABs
  *.cab                    carved CAB payloads

ghidra/                 Ghidra 12.1.2 headless analysis
  proj/                    project "ThermalCam"
  out/libthermometry.so.c  decompiled C (36 KB)   <- fully understood
  out/libuvc.so.c          decompiled C (496 KB)
  out/libDYTJpegAes.so.c   decompiled C (1.3 MB)
  out/libUVCCamera.so.c    decompiled C (5.5 MB)  <- the JNI layer
  scripts/ExportDecompiled.java   exports decompiled C for all functions

tools/
  nsis_extract.py          reusable NSIS extractor (see 02 §2.4)

logs/                   tool logs
```

## Quick start

```bash
# Re-extract the Windows payload
python3 tools/nsis_extract.py \
    "../iScout Mechanic-Ti VisualPlatformSetUp v3.0.6+windows/iScout Mechanic-Ti VisualPlatformSetUp v3.0.6.exe" \
    exe/nsis_blocks --max-size 4000000

# Re-decompile a native library
GH=/home/shaanair/Projects/Ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless
"$GH" ghidra/proj ThermalCam -process libthermometry.so -noanalysis \
      -scriptPath ghidra/scripts \
      -postScript ExportDecompiled.java ghidra/out/libthermometry.so.c
```

## Start here

`../RE Docs/README.md`

## Key results

* USB protocol: `../RE Docs/04-usb-protocol.md`
* Thermometry math + constants: `../RE Docs/05-thermometry-algorithm.md`
* What is still unknown: `../RE Docs/09-open-questions-and-next-steps.md`
