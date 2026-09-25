#!/bin/sh
# check.sh — verify the packaging metadata against the app it describes.
#
# Run from the port root (the Makefile's `check` target does).  These are
# cross-file invariants that a syntax linter cannot see: the desktop entry's
# Exec, Icon and StartupWMClass must name the installed binary, the icon the
# build ships, and the WM_CLASS the app actually sets.  A typo in any of them
# validates cleanly and still launches nothing, which is the failure this
# catches.
#
# The super-resolution payload is checked the same way: the model has to land
# where the app's own search looks, and a binary that links libMNN.so has to
# ship the library at the one path its $ORIGIN rpath resolves to.  Both are
# silent failures — the first looks like a build without a runtime, the second
# makes the installed app fail to start.
#
# Exits non-zero on a real mismatch.  The optional tools (desktop-file-validate,
# ImageMagick, readelf) are used when present and reported as skipped when not,
# so the check still means something on a host that lacks them.
set -u

desktop=packaging/dytqt.desktop
svg=packaging/dytqt.svg
fails=0

ok()   { printf '  ok   %s\n' "$1"; }
fail() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }
skip() { printf '  --   %s (skipped: %s)\n' "$1" "$2"; }

if [ ! -f "$desktop" ] || [ ! -f "$svg" ]; then
    echo "packaging/check.sh: run from the port root" >&2
    exit 2
fi

echo "== packaging metadata =="

# The value of a key in the [Desktop Entry] group, or empty.
key() {
    awk -v k="$1" '
        /^\[Desktop Entry\]/ { inb = 1; next }
        /^\[/                { inb = 0 }
        inb && index($0, k "=") == 1 { sub("^" k "=", ""); print; exit }
    ' "$desktop"
}

# --- the desktop entry ---
if command -v desktop-file-validate >/dev/null 2>&1; then
    # The only expected diagnostic is the advisory "more than one main
    # category" hint, which Graphics+Science always produces; anything else is
    # a real problem.
    out=$(desktop-file-validate "$desktop" 2>&1 | grep -v ': hint:' || true)
    if [ -z "$out" ]; then
        ok "the desktop entry validates"
    else
        printf '%s\n' "$out"
        fail "the desktop entry validates"
    fi
else
    skip "the desktop entry validates" "desktop-file-validate absent"
fi

exec_name=$(key Exec)
icon_name=$(key Icon)
wmclass=$(key StartupWMClass)

if [ "$exec_name" = "dytqt" ]; then
    ok "Exec names the installed binary ($exec_name)"
else
    fail "Exec names the installed binary (got '${exec_name:-<missing>}')"
fi

# Qt derives WM_CLASS from the application name, which defaults to the
# executable's basename, so this must equal Exec.  Measured on this host:
# WM_CLASS = ("dytqt", "dytqt").
if [ -n "$wmclass" ] && [ "$wmclass" = "$exec_name" ]; then
    ok "StartupWMClass matches the app's WM_CLASS ($wmclass)"
else
    fail "StartupWMClass matches the app's WM_CLASS (got '${wmclass:-<missing>}')"
fi

# Icon names a basename under hicolor; its source must exist beside the entry.
if [ "$icon_name" = "dytqt" ] && [ -f "packaging/$icon_name.svg" ]; then
    ok "Icon names a source the build ships ($icon_name.svg)"
else
    fail "Icon names a source the build ships (got '${icon_name:-<missing>}')"
fi

# --- the icon ---
if command -v magick >/dev/null 2>&1 || command -v convert >/dev/null 2>&1; then
    im=$(command -v magick || command -v convert)
    tmp=$(mktemp -d)
    good=1
    for sz in 16 256; do
        if ! "$im" -background none "$svg" -resize "${sz}x${sz}" \
                "$tmp/i-$sz.png" >/dev/null 2>&1; then
            good=0
            continue
        fi
        # A blank render is the failure mode of an unsupported SVG feature.
        mean=$("$im" "$tmp/i-$sz.png" -format '%[fx:mean]' info: 2>/dev/null \
               || echo 0)
        case "$mean" in 0 | 0.0*) good=0 ;; esac
    done
    rm -rf "$tmp"
    if [ "$good" = 1 ]; then
        ok "the icon rasterises at 16 and 256 (non-blank)"
    else
        fail "the icon rasterises at 16 and 256 (non-blank)"
    fi
else
    skip "the icon rasterises" "ImageMagick absent"
fi

# --- the super-resolution payload ---
#
# Two cross-file invariants that fail silently otherwise.  The app searches for
# the model under <datadir>/dytqt/models/zoom2.mnn (dyt_vm_find_model), so
# `make install` has to put it exactly there; get that wrong and an installed
# app reports no model and the SR keys refuse — which is indistinguishable from
# a build without the runtime, and so never looks like a packaging bug.  And a
# binary that links libMNN.so must ship the library at the one place its
# $ORIGIN rpath looks, or the installed app will not start at all.

model=models/zoom2.mnn
msize=$(wc -c < "$model" 2>/dev/null | tr -d ' ')
if [ -n "$msize" ] && [ "$msize" -gt 1000 ]; then
    ok "the super-resolution model ships ($model, $msize bytes)"
else
    fail "the super-resolution model ships (got '${msize:-missing}' bytes)"
fi

# The model's directory as the search spells it, and the file name it appends.
if grep -q 'dytqt/models' Makefile && grep -q 'zoom2\.mnn' Makefile; then
    ok "install puts the model where the app searches for it"
else
    fail "install puts the model where the app searches for it"
fi

if [ -f build/dytqt ] && command -v readelf >/dev/null 2>&1; then
    runpath=$(readelf -d build/dytqt 2>/dev/null \
              | sed -n 's/.*(RUNPATH).*\[\(.*\)\].*/\1/p')
    if readelf -d build/dytqt 2>/dev/null | grep -q 'libMNN'; then
        case "$runpath" in
            *'$ORIGIN'*'../lib/dytqt'*)
                ok "dytqt links libMNN.so and its rpath finds the shipped copy" ;;
            *)
                fail "dytqt links libMNN.so and its rpath finds the shipped copy (runpath '${runpath:-<none>}')" ;;
        esac
        if grep -q 'libMNN\.so' Makefile; then
            ok "install ships the libMNN.so the binary needs"
        else
            fail "install ships the libMNN.so the binary needs"
        fi
        # An absolute path into the build tree would be shipped in the .deb —
        # lintian's binary-or-shlib-defines-rpath, and a way for a target
        # machine to load a library the package does not contain.
        if readelf -d build/dytqt 2>/dev/null | grep -qF "$(pwd)"; then
            fail "the rpath carries no path into the build tree"
        else
            ok "the rpath carries no path into the build tree"
        fi
    else
        skip "dytqt links the shipped libMNN.so" "this build has no MNN"
    fi
else
    skip "dytqt links the shipped libMNN.so" "build/dytqt absent or readelf missing"
fi

if [ "$fails" -ne 0 ]; then
    echo "== packaging metadata FAILED =="
    exit 1
fi
echo "== packaging metadata ok =="
exit 0
