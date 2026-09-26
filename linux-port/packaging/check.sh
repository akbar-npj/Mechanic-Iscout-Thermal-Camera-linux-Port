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
# ship the library at every path its $ORIGIN rpath resolves to — one per package
# format.  Both are silent failures — the first looks like a build without a
# runtime, the second makes the installed app fail to start.  The RPM spec is
# held to the same standard: it packages what `make install` stages, so the two
# have to agree about where the library goes, whether it ships at all, and the
# version.
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

# The name a person reads lives in two files: the menu entry's Name, and the
# window title and About box, which share one constant.  A rename that touches
# one and not the other is invisible until someone opens the menu.
app_name=$(key Name)
if [ -n "$app_name" ] && grep -qF "kAppName[] = \"$app_name\"" gui/dytqt.cpp; then
    ok "the menu name and the window title are one name ($app_name)"
else
    fail "the menu name and the window title are one name (menu '${app_name:-<missing>}')"
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
        # Each package puts the library one directory up and over from the
        # binary — the deb under $(PREFIX)/lib, the rpm under Fedora's
        # $(PREFIX)/lib64 — so the rpath has to carry both, or one of the two
        # packages installs a binary that cannot find its own runtime.
        have_lib=0
        have_lib64=0
        case "$runpath" in *'$ORIGIN/../lib/dytqt'*)   have_lib=1 ;; esac
        case "$runpath" in *'$ORIGIN/../lib64/dytqt'*) have_lib64=1 ;; esac
        if [ "$have_lib" = 1 ] && [ "$have_lib64" = 1 ]; then
            ok "dytqt links libMNN.so and its rpath finds both packaged copies"
        else
            fail "dytqt links libMNN.so and its rpath finds both packaged copies (runpath '${runpath:-<none>}')"
        fi
        if grep -q 'libMNN\.so' Makefile; then
            ok "install ships the libMNN.so the binary needs"
        else
            fail "install ships the libMNN.so the binary needs"
        fi
        # An absolute path into the build tree would be shipped in a package —
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

# --- the RPM metadata ---
#
# packaging/rpm/dytqt.spec packages what `make install` stages, so the two have
# to agree about the three things that can silently diverge: where the private
# library goes, whether it ships at all, and the version.  A mismatch does not
# fail the build — it produces a package that installs a binary unable to find
# its runtime, or a libMNN.so nothing loads.
spec=packaging/rpm/dytqt.spec
if [ -f "$spec" ]; then
    if grep -q '^Name:[[:space:]]*dytqt' "$spec"; then
        ok "the spec names the package dytqt"
    else
        fail "the spec names the package dytqt"
    fi
    # The binary's $ORIGIN/../lib64/dytqt entry is what finds it, so the spec
    # must install the library under %{_libdir}/dytqt — the same relative
    # position `make install` uses when LIBDIR is rpm's %{_libdir}.
    if grep -q '%{_libdir}/dytqt/libMNN\.so' "$spec"; then
        ok "the spec ships libMNN.so where the binary looks for it"
    else
        fail "the spec ships libMNN.so where the binary looks for it"
    fi
    # `make rpm` passes --with/--without mnn from the same HAVE_MNN that decides
    # whether `make install` ships the library, so the two cannot disagree.
    if grep -q '%if %{with mnn}' "$spec" \
       && grep -q 'RPM_WITH.*HAVE_MNN' Makefile; then
        ok "the spec's MNN switch is the Makefile's HAVE_MNN"
    else
        fail "the spec's MNN switch is the Makefile's HAVE_MNN"
    fi
    if grep -q 'Version:[[:space:]]*%{_dytqt_version}' "$spec" \
       && grep -q '_dytqt_version.*\$(VERSION)' Makefile; then
        ok "the spec's version comes from the Makefile's VERSION"
    else
        fail "the spec's version comes from the Makefile's VERSION"
    fi
    if grep -q 'rpmbuild' Makefile && grep -q 'packaging/rpm/dytqt\.spec' Makefile; then
        ok "the Makefile builds the spec"
    else
        fail "the Makefile builds the spec"
    fi
else
    skip "the RPM spec ships" "packaging/rpm/dytqt.spec absent"
fi

# --- the udev rule ---
#
# The rule is what lets a non-root desktop user open the camera, and it ships
# in both packages.  Three things can rot silently: the rule's device list can
# drift from the one the code actually probes for (tools/probe.c's known[]), so
# a camera the app supports stays unopenable; the file can be installed but
# never claimed by a package's file list, so rpm leaves it behind on uninstall;
# and neither package can reload udev, so the rule only takes effect after a
# reboot.  None of these is a build error, which is why they are checked here.
rules=packaging/Mechanic-iScout-Thermal-Camera.rules
if [ -f "$rules" ]; then
    # The device list, normalised to "vid:pid" and sorted, from both sides.
    # probe.c's known[] is the authority: it is what the app offers to open.
    from_probe=$(sed -n \
        's/^[[:space:]]*{[[:space:]]*0x\([0-9a-fA-F]*\),[[:space:]]*0x\([0-9a-fA-F]*\),.*/\1:\2/p' \
        tools/probe.c | tr '[:upper:]' '[:lower:]' | sort)
    from_rules=$(sed -n \
        's/.*idVendor}=="\([0-9a-fA-F]*\)".*idProduct}=="\([0-9a-fA-F]*\)".*/\1:\2/p' \
        "$rules" | tr '[:upper:]' '[:lower:]' | sort)
    if [ -n "$from_rules" ] && [ "$from_probe" = "$from_rules" ]; then
        n=$(printf '%s\n' "$from_rules" | wc -l | tr -d ' ')
        ok "the udev rule lists the same $n devices the code probes for"
    else
        fail "the udev rule lists the same devices the code probes for"
        printf '       probe.c: %s\n' "$(printf '%s' "$from_probe" | tr '\n' ' ')"
        printf '       rule:    %s\n' "$(printf '%s' "$from_rules" | tr '\n' ' ')"
    fi

    # `make install` is the one staging path both packages share, so the rule
    # has to be in it — that is what puts it in the deb and in the rpm's stage.
    if grep -q 'RULES_NAME' Makefile && grep -q 'udev/rules\.d' Makefile \
       && grep -q 'packaging/\$(RULES_NAME)' Makefile; then
        ok "install ships the udev rule"
    else
        fail "install ships the udev rule"
    fi

    # rpm refuses to own a file it was not told about, but a file staged and
    # *not* listed is only a build failure because Fedora's unpackaged-files
    # check is fatal; on a laxer host it would install an unowned file.
    if [ -f "$spec" ] \
       && grep -q '%{_udevrulesdir}/Mechanic-iScout-Thermal-Camera\.rules' "$spec"; then
        ok "the spec claims the udev rule in its file list"
    else
        fail "the spec claims the udev rule in its file list"
    fi

    # A rule that is installed but never reloaded is inert until reboot, and
    # the packaging macro that would do it expands to nothing on Fedora, so
    # both packages must call udevadm themselves.
    if [ -f "$spec" ] && grep -q 'udevadm control --reload-rules' "$spec"; then
        ok "the spec reloads udev after installing the rule"
    else
        fail "the spec reloads udev after installing the rule"
    fi
    if [ -f packaging/deb/postinst ] \
       && grep -q 'udevadm control --reload-rules' packaging/deb/postinst \
       && grep -q 'install -m 0755 packaging/deb/postinst' Makefile; then
        ok "the deb reloads udev after installing the rule"
    else
        fail "the deb reloads udev after installing the rule"
    fi
else
    skip "the udev rule ships" "$rules absent"
fi

# --- the optional Qt OpenGL module ---
#
# The 3D Analysis view compiles a GL renderer only when the Makefile found
# Qt6OpenGLWidgets, and the two have to agree about the macro that decides it.
# A rename on either side does not fail the build: the GL class simply stops
# being compiled, every host silently falls back to the software renderer, and
# the GUI selftest still passes (it asserts the fallback, and its backend check
# is `using_gl() == gl_ready()` — true in both worlds).  So the agreement is
# checked here, where the Makefile is visible.
#
# The module is optional on purpose — without it the GUI still builds and the
# tab still draws — which is why this is a coherence check, not a requirement.
if grep -q 'Qt6OpenGLWidgets' Makefile \
   && grep -q 'DYT_HAVE_QT6_OPENGL' Makefile \
   && grep -q 'DYT_HAVE_QT6_OPENGL' gui/dytqt.cpp; then
    ok "the optional Qt OpenGL module is wired through one macro"
else
    fail "the optional Qt OpenGL module is wired through one macro"
fi

# The deb's fallback Depends has to name the OpenGL packages too, or a host
# that falls back to it (no dpkg-shlibdeps) installs a binary whose GL library
# nothing pulls in.  The measured path finds them on its own.
if grep -q 'libqt6openglwidgets6' Makefile; then
    ok "the deb's fallback Depends names the Qt OpenGL packages"
else
    fail "the deb's fallback Depends names the Qt OpenGL packages"
fi

if [ "$fails" -ne 0 ]; then
    echo "== packaging metadata FAILED =="
    exit 1
fi
echo "== packaging metadata ok =="
exit 0
