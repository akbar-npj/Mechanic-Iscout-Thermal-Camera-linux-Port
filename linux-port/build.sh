#!/usr/bin/env bash
# build.sh — automated build and test driver for the DYT thermal camera Linux port
#
# Usage:
#   ./build.sh [OPTIONS] [TARGETS...]
#
# TARGETS (default: build check)
#   build       compile everything (make)
#   check       run the regression gate (make check)
#   deb         build a .deb package  (needs dpkg-deb)
#   rpm         build an .rpm package (needs rpmbuild)
#   clean       remove the build directory
#   install     install to PREFIX (default /usr/local)
#
# OPTIONS
#   --jobs N        parallel build jobs (default: nproc)
#   --jpeg libjpeg  use libjpeg instead of the vendored stb codec
#   --mnn ROOT      path to an MNN install (default: third_party/mnn-install)
#   --prefix DIR    install prefix (default: /usr/local)
#   --no-mnn-stub   fail the build when no MNN runtime is found
#   --version VER   override the package version (default from Makefile)
#   --color         force ANSI colour even when stdout is not a terminal
#   --no-color      disable ANSI colour
#   -v / --verbose  print every make command as it runs (make V=1)
#   -h / --help     show this message and exit
#
# Package selection
#   When multiple packages of the same type (deb/rpm) exist in build/ the
#   script selects the one with the NEWEST timestamp, so a re-run after a
#   version bump does not accidentally verify an old artefact.
#
# Exit codes
#   0   all requested targets succeeded
#   1   one or more targets failed (the failing target is printed)
#   2   bad usage / missing prerequisite
#
# MANDATORY flag: -ffp-contract=off
#   The thermometry port reproduces libthermometry.so byte-for-byte only with
#   FP contraction disabled.  Do not remove it from CFLAGS.  See BUILDING.md
#   and RE Docs/08 §8.6.1.

set -euo pipefail

# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------
_RESET='' _BOLD='' _GREEN='' _YELLOW='' _RED='' _CYAN=''
if [ -t 1 ] || [ "${BUILD_COLOR:-}" = "1" ]; then
    _RESET=$'\033[0m'
    _BOLD=$'\033[1m'
    _GREEN=$'\033[0;32m'
    _YELLOW=$'\033[0;33m'
    _RED=$'\033[0;31m'
    _CYAN=$'\033[0;36m'
fi

info()    { printf '%b[build]%b %s\n' "${_CYAN}${_BOLD}" "${_RESET}" "$*"; }
ok()      { printf '%b[  OK ]%b %s\n' "${_GREEN}${_BOLD}" "${_RESET}" "$*"; }
warn()    { printf '%b[ WARN]%b %s\n' "${_YELLOW}${_BOLD}" "${_RESET}" "$*" >&2; }
fail()    { printf '%b[ FAIL]%b %s\n' "${_RED}${_BOLD}" "${_RESET}" "$*" >&2; }
die()     { fail "$*"; exit 1; }
step()    { printf '\n%b==> %s%b\n' "${_BOLD}" "$*" "${_RESET}"; }

# ---------------------------------------------------------------------------
# Script location — always run from linux-port/
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
JOBS=$(nproc 2>/dev/null || echo 1)
JPEG_BACKEND=""          # empty → stb (Makefile default)
MNN_ROOT=""              # empty → Makefile decides (third_party/mnn-install)
PREFIX="/usr/local"
VERSION_OVERRIDE=""
VERBOSE=0
REQUIRE_MNN=0
TARGETS=()

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)       JOBS="$2";           shift 2 ;;
        --jpeg)       JPEG_BACKEND="$2";   shift 2 ;;
        --mnn)        MNN_ROOT="$2";       shift 2 ;;
        --prefix)     PREFIX="$2";         shift 2 ;;
        --no-mnn-stub) REQUIRE_MNN=1;      shift ;;
        --version)    VERSION_OVERRIDE="$2"; shift 2 ;;
        --color)      BUILD_COLOR=1; _RESET=$'\033[0m'; _BOLD=$'\033[1m'
                      _GREEN=$'\033[0;32m'; _YELLOW=$'\033[0;33m'
                      _RED=$'\033[0;31m';  _CYAN=$'\033[0;36m'; shift ;;
        --no-color)   _RESET='' _BOLD='' _GREEN='' _YELLOW='' _RED='' _CYAN=''; shift ;;
        -v|--verbose) VERBOSE=1;           shift ;;
        -h|--help)
            sed -n '3,/^set -/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        build|check|deb|rpm|clean|install)
            TARGETS+=("$1"); shift ;;
        *)
            die "Unknown option or target: '$1'.  Use --help for usage." ;;
    esac
done

# Default target set when nothing was requested
[[ ${#TARGETS[@]} -eq 0 ]] && TARGETS=(build check)

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
step "Checking prerequisites"

require_cmd() {
    local cmd="$1" pkg="${2:-$1}"
    if ! command -v "$cmd" &>/dev/null; then
        die "'$cmd' is required but not found.  Install: $pkg"
    fi
}

require_cmd make  make
require_cmd cc    "gcc or clang"
require_cmd ar    binutils

# Warn (do not abort) about optional tools
for opt in pkg-config dpkg-deb rpmbuild; do
    command -v "$opt" &>/dev/null && ok "$opt found" \
        || warn "$opt not found (optional; some targets may be unavailable)"
done

# Verify the mandatory -ffp-contract=off is still in the Makefile
if ! grep -q -- '-ffp-contract=off' Makefile; then
    die "Makefile no longer contains -ffp-contract=off.  This flag is MANDATORY
    for byte-exact thermometry reproduction.  See BUILDING.md §Build."
fi
ok "Mandatory -ffp-contract=off present in Makefile"

# Verify MNN requirement
if [[ $REQUIRE_MNN -eq 1 ]]; then
    root="${MNN_ROOT:-third_party/mnn-install}"
    if [[ ! -f "$root/include/MNN/Interpreter.hpp" ]] \
    || [[ ! -f "$root/lib/libMNN.so" ]]; then
        die "--no-mnn-stub requested but no MNN runtime found at '$root'.
    See linux-port/third_party/README.md for build instructions."
    fi
fi

# ---------------------------------------------------------------------------
# Build the make command-line extras from options
# ---------------------------------------------------------------------------
MAKE_EXTRA=()
[[ -n "$JPEG_BACKEND" ]]    && MAKE_EXTRA+=("JPEG=$JPEG_BACKEND")
[[ -n "$MNN_ROOT" ]]        && MAKE_EXTRA+=("MNN_ROOT=$MNN_ROOT")
[[ -n "$VERSION_OVERRIDE" ]] && MAKE_EXTRA+=("VERSION=$VERSION_OVERRIDE")
[[ -n "$PREFIX" ]]          && MAKE_EXTRA+=("PREFIX=$PREFIX")
[[ $VERBOSE -eq 1 ]]        && MAKE_EXTRA+=("V=1")

MAKE_CMD=(make -j"$JOBS" "${MAKE_EXTRA[@]+"${MAKE_EXTRA[@]}"}")

info "make command: ${MAKE_CMD[*]}"
info "Targets: ${TARGETS[*]}"

# ---------------------------------------------------------------------------
# Helper: pick the newest file matching a glob
# ---------------------------------------------------------------------------
# Usage: newest_file GLOB
# Prints the path of the newest matching file, or nothing if none match.
newest_file() {
    local glob="$1"
    # Use ls -t (newest first) and take the first match.
    # Disable glob expansion failure in case there are no matches.
    local files
    files=$(ls -t $glob 2>/dev/null | head -1) || true
    printf '%s\n' "$files"
}

# ---------------------------------------------------------------------------
# Failure tracker
# ---------------------------------------------------------------------------
FAILED_TARGETS=()

run_target() {
    local target="$1"
    case "$target" in

        # ----------------------------------------------------------------
        build)
            step "Building (make -j$JOBS)"
            if "${MAKE_CMD[@]}"; then
                ok "build succeeded"
            else
                fail "build FAILED"
                FAILED_TARGETS+=(build)
            fi
            ;;

        # ----------------------------------------------------------------
        check)
            step "Running regression gate (make check)"
            if "${MAKE_CMD[@]}" check; then
                ok "all checks PASSED"
            else
                fail "make check FAILED — see output above"
                FAILED_TARGETS+=(check)
            fi
            ;;

        # ----------------------------------------------------------------
        deb)
            step "Building Debian package (make deb)"
            require_cmd dpkg-deb "dpkg / dpkg-dev"
            if "${MAKE_CMD[@]}" deb; then
                # Select the newest .deb in build/ in case older ones exist
                pkg=$(newest_file "build/*.deb")
                if [[ -n "$pkg" ]]; then
                    ok "deb built: $pkg"
                    info "Package info:"
                    dpkg-deb --info "$pkg" | sed 's/^/  /'
                else
                    warn "make deb succeeded but no .deb found in build/"
                fi
            else
                fail "make deb FAILED"
                FAILED_TARGETS+=(deb)
            fi
            ;;

        # ----------------------------------------------------------------
        rpm)
            step "Building RPM package (make rpm)"
            require_cmd rpmbuild "rpm-build"
            if "${MAKE_CMD[@]}" rpm; then
                # Select the newest .rpm in build/ in case older ones exist
                pkg=$(newest_file "build/*.rpm")
                if [[ -n "$pkg" ]]; then
                    ok "rpm built: $pkg"
                    info "Package info:"
                    rpm -qp --qf 'name=%{NAME} version=%{VERSION}-%{RELEASE} arch=%{ARCH}\n' \
                        "$pkg" | sed 's/^/  /'
                else
                    warn "make rpm succeeded but no .rpm found in build/"
                fi
            else
                fail "make rpm FAILED"
                FAILED_TARGETS+=(rpm)
            fi
            ;;

        # ----------------------------------------------------------------
        clean)
            step "Cleaning build directory (make clean)"
            if "${MAKE_CMD[@]}" clean; then
                ok "clean done"
            else
                fail "make clean FAILED"
                FAILED_TARGETS+=(clean)
            fi
            ;;

        # ----------------------------------------------------------------
        install)
            step "Installing to $PREFIX (make install)"
            if "${MAKE_CMD[@]}" install; then
                ok "installed to $PREFIX"
                info "To activate the udev camera rule without a reboot:"
                info "  sudo udevadm control --reload-rules"
                info "  sudo udevadm trigger --subsystem-match=usb"
            else
                fail "make install FAILED"
                FAILED_TARGETS+=(install)
            fi
            ;;

        *)
            die "Internal error: unhandled target '$target'"
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Run all requested targets in order
# ---------------------------------------------------------------------------
for t in "${TARGETS[@]}"; do
    run_target "$t"
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
if [[ ${#FAILED_TARGETS[@]} -eq 0 ]]; then
    ok "All targets completed successfully: ${TARGETS[*]}"
    exit 0
else
    fail "The following targets FAILED: ${FAILED_TARGETS[*]}"
    exit 1
fi
