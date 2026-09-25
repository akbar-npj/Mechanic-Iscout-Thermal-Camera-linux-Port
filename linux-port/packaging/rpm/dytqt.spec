# dytqt.spec — RPM packaging for the DYT/Mechanic-Ti thermal camera viewer.
#
# This spec only *packages*; it compiles nothing.  `make rpm` builds the tree,
# stages it with `make install`, and passes the staged directory in as the
# _stage macro; the install script below copies that into the buildroot.  The
# RPM therefore installs exactly what `make install` does — the same guarantee
# packaging/deb gets by staging through `make install` — and rpmbuild's own
# check turns a file that `make install` staged but the file list does not
# mention into a build failure, because Fedora sets the unpackaged-files check
# to terminate the build.
#
# There is deliberately no Source: and no prep or build script: the source is
# the working tree, and a tarball would only re-introduce the path handling
# this design avoids (this tree's own path contains a space).
#
# The comments avoid bare percent signs on purpose.  rpm expands macros inside
# comments, so a comment naming a macro is at best a warning and at worst —
# when the macro's body spans lines — shell text injected into the script.

%bcond_without mnn

# The tree is pre-built and pre-staged, so a debuginfo subpackage would only
# re-strip what the Makefile already produced and add a second RPM.
%global debug_package %{nil}

# Overridden by `make rpm` with --define, so the spec and the Makefile cannot
# disagree about the version.  The default keeps a bare rpmbuild parseable.
%{!?_dytqt_version:%global _dytqt_version 0.1.0}

Name:           dytqt
Version:        %{_dytqt_version}
Release:        1%{?dist}
Summary:        Viewer and recorder for DYT/Mechanic-Ti USB thermal cameras

# GPLv3 *only*: LICENSE is the unmodified GPLv3 text, packaging/deb/copyright
# says "version 3", and no source header grants "or later".
License:        GPL-3.0-only

# The icons land in the hicolor tree, so pull in its index.
Requires:       hicolor-icon-theme

%description
dytqt views and records from a DYT/Mechanic-Ti USB thermal camera.  It
renders radiometric frames through the vendor's palettes, measures
point/line/ROI temperatures with the same thermometry the device uses,
and writes stills (DYT container) and clips (mp4).

It is a clean-room Linux port, not a vendor product, and shares no code
with the Windows application.

%prep
# Nothing: the tree is already built and staged by `make rpm`.

%build
# Nothing: `make rpm` already built the binary.

%install
# Both paths are passed in by `make rpm`.  They are quoted because this tree's
# path contains a space ("Thermal Camera") and rpm does not quote macro
# expansions; the install preamble has already changed directory by here.
%{!?_stage:%{error: pass --define '_stage <directory staged by make install>'}}
%{!?_license:%{error: pass --define '_license <path to the LICENSE file>'}}
cp -a "%{_stage}/." "%{buildroot}/"

# The licence is not part of `make install` (the deb ships its own DEP-5
# copyright instead), so it is installed here.  The license directive below
# uses the absolute-path form: the relative form copies from the build
# directory, which is empty for a pre-staged build.
install -d "%{buildroot}%{_datadir}/licenses/%{name}"
install -m 0644 "%{_license}" "%{buildroot}%{_datadir}/licenses/%{name}/LICENSE"

%files
%{_bindir}/dytqt
%{_datadir}/applications/dytqt.desktop

# The palettes are not decoration: the engine falls back to six built-in ramps
# when it cannot find them, so a package missing them would silently offer 6 of
# the 28.  dyt_vm_find_data_dir() looks under datadir/dytqt/<leaf> in each
# XDG_DATA_DIRS entry.  The model is found by the same search.
%dir %{_datadir}/dytqt
%dir %{_datadir}/dytqt/palettes
%{_datadir}/dytqt/palettes/*.dat
%dir %{_datadir}/dytqt/models
%{_datadir}/dytqt/models/zoom2.mnn

# The hicolor tree itself belongs to hicolor-icon-theme, so only the files are
# listed here — not the size directories.
%{_datadir}/icons/hicolor/16x16/apps/dytqt.png
%{_datadir}/icons/hicolor/24x24/apps/dytqt.png
%{_datadir}/icons/hicolor/32x32/apps/dytqt.png
%{_datadir}/icons/hicolor/48x48/apps/dytqt.png
%{_datadir}/icons/hicolor/64x64/apps/dytqt.png
%{_datadir}/icons/hicolor/128x128/apps/dytqt.png
%{_datadir}/icons/hicolor/256x256/apps/dytqt.png

# The licenses directory is owned by filesystem, so only the package's own
# subdirectory is claimed.
%dir %{_datadir}/licenses/%{name}
%license %{_datadir}/licenses/%{name}/LICENSE

# The private MNN runtime, shipped only when the build linked one.  `make rpm`
# passes --with mnn iff the Makefile's HAVE_MNN is set, so this block and
# `make install` cannot disagree.  Its unversioned SONAME is fine here: the ELF
# generator emits a Provides for it, which satisfies the Requires the same
# generator derives from the binary.  libMNN_Express.so is not shipped because
# nothing links it.
%if %{with mnn}
%dir %{_libdir}/dytqt
%{_libdir}/dytqt/libMNN.so
%endif

%changelog
* Fri Sep 25 2026 dytqt port <noreply@example.invalid> - 0.1.0-1
- Initial RPM: packages what `make install` stages — the binary, desktop entry,
  icons, the 28 vendor palettes, the super-resolution model, and (when built
  with MNN) the private libMNN.so under the libdir.
