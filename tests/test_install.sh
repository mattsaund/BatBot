#!/usr/bin/env bash
# Tests for install.sh's pure logic: version comparison, the CUDA-vs-hardware
# table, and CMake version detection. Getting these wrong means a GPU that
# silently cannot be used, so they are worth pinning down.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRUCIBLE_INSTALL_LIB=1
export CRUCIBLE_INSTALL_LIB
# shellcheck disable=SC1091
source "$HERE/../install.sh"

PASS=0
FAIL=0

check() {
    local what="$1"; shift
    if "$@"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s\n' "$what"
    fi
}

check_not() {
    local what="$1"; shift
    if "$@"; then
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s (expected false)\n' "$what"
    else
        PASS=$((PASS + 1))
    fi
}

check_eq() {
    local what="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s\n         got: %s\n    expected: %s\n' "$what" "$actual" "$expected"
    fi
}

echo "install.sh logic tests"
echo

echo "  version_ge"
check     "12.8 >= 12.0"  version_ge 12.8 12.0
check     "12.0 >= 12.0"  version_ge 12.0 12.0
check_not "12.0 >= 12.8"  version_ge 12.0 12.8
check     "13.0 >= 12.8"  version_ge 13.0 12.8
check_not "11.8 >= 12.0"  version_ge 11.8 12.0
check     "3.28 >= 3.24"  version_ge 3.28 3.24
check_not "3.22 >= 3.24"  version_ge 3.22 3.24
check     "3.5  >= 3.5"   version_ge 3.5 3.5
# Minor versions must compare numerically, not as text: "3.9" > "3.10" only if
# you are comparing strings.
check     "3.10 >= 3.9"   version_ge 3.10 3.9
check_not "3.9 >= 3.10"   version_ge 3.9 3.10

echo "  cuda_required_for_cap"
# Blackwell (RTX 50-series) is the case that catches people out: the toolkit
# most distributions ship cannot target it at all.
check_eq "sm_120 Blackwell" "$(cuda_required_for_cap 12.0)" "12.8"
check_eq "sm_90  Hopper"    "$(cuda_required_for_cap 9.0)"  "12.0"
check_eq "sm_89  Ada"       "$(cuda_required_for_cap 8.9)"  "11.8"
check_eq "sm_86  Ampere"    "$(cuda_required_for_cap 8.6)"  "11.1"
check_eq "sm_75  Turing"    "$(cuda_required_for_cap 7.5)"  "11.0"

echo "  the toolkit a distro ships must be rejected for newer cards"
# The exact situation on a machine with an RTX 5060 Ti and Ubuntu's CUDA 12.0.
check_not "CUDA 12.0 cannot build for sm_120" \
    version_ge "12.0" "$(cuda_required_for_cap 12.0)"
check     "CUDA 12.8 can build for sm_120" \
    version_ge "12.8" "$(cuda_required_for_cap 12.0)"
check     "CUDA 12.0 is fine for an RTX 4070" \
    version_ge "12.0" "$(cuda_required_for_cap 8.9)"

echo "  cmake_version_ok"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
make_fake_cmake() {
    printf '#!/bin/sh\necho "cmake version %s"\n' "$1" > "$TMP/cmake"
    chmod +x "$TMP/cmake"
}
make_fake_cmake "3.28.3"; check     "3.28.3 accepted" cmake_version_ok "$TMP/cmake"
make_fake_cmake "3.24.0"; check     "3.24.0 accepted" cmake_version_ok "$TMP/cmake"
make_fake_cmake "3.22.1"; check_not "3.22.1 rejected" cmake_version_ok "$TMP/cmake"
make_fake_cmake "4.0.1";  check     "4.0.1 accepted"  cmake_version_ok "$TMP/cmake"
check_not "a missing cmake is rejected" cmake_version_ok "$TMP/definitely-not-here"

# --------------------------------------------------------------------------
# Package lists
#
# The Vulkan build fails at configure time without SPIR-V headers, several
# minutes in, with an error naming a CMake package rather than anything you
# can install. That was a real bug; this is the check that it stays fixed.
# --------------------------------------------------------------------------
echo
echo "  resolve_packages"

PKG=apt RUNTIME=vulkan resolve_packages
check     "apt vulkan includes spirv-headers" \
          grep -q "spirv-headers" <<< "${PKGS_VULKAN[*]}"
check     "apt vulkan includes glslc" \
          grep -q "glslc" <<< "${PKGS_VULKAN[*]}"
check     "apt vulkan includes libvulkan-dev" \
          grep -q "libvulkan-dev" <<< "${PKGS_VULKAN[*]}"
check     "apt base includes a compiler" \
          grep -q "build-essential" <<< "${PKGS_BASE[*]}"

for manager in dnf pacman zypper; do
    PKG="$manager" resolve_packages
    check "$manager vulkan names a SPIR-V headers package" \
          grep -qi "spirv" <<< "${PKGS_VULKAN[*]}"
    check "$manager names a CUDA package" \
          test -n "${PKGS_CUDA[*]}"
done

# resolve_packages ends in a `case`, and a stray non-zero exit there would
# abort the caller under `set -e`. This is the bug that once silently killed
# --check, so it is pinned.
PKG=apt RUNTIME=cpu resolve_packages
check_eq  "resolve_packages succeeds for a CPU install" "$?" "0"

# --------------------------------------------------------------------------
# Build directory
#
# Loadable runtimes are shared libraries, which need symlinks. A checkout on
# exFAT cannot hold them, so the build tree has to move.
# --------------------------------------------------------------------------
echo
echo "  choose_build_dir"

SRC_DIR="$TMP/checkout"
mkdir -p "$SRC_DIR"
choose_build_dir
check_eq  "the build tree sits beside the source" "$BUILD_DIR" "$SRC_DIR/build"

# --------------------------------------------------------------------------
# Symlink-free shared libraries
#
# llama.cpp writes libggml-base.so.0.9.4 with libggml-base.so as a symlink
# beside it, and neither exFAT nor NTFS can hold a symlink -- so a checkout on
# an external drive shared with Windows could not be built in. The installer
# used to detect that and move the build tree into the cache directory. It no
# longer has to, because Crucible strips the version suffixes and the libraries
# come out as plain files; these pin that the detection stays gone and the
# stripping stays wired up, in both places that compile llama.cpp.
# --------------------------------------------------------------------------
# Re-running the installer over an old build tree is now the ordinary case,
# not a rare one: the build stays in the checkout instead of being relocated to
# a fresh cache directory. A CMakeCache remembering paths this tree no longer
# has -- a moved home, a restored backup, a source directory pointed elsewhere
# once -- makes cmake refuse to reuse it, and the cache is pure derived data,
# so the installer clears it and tries once more rather than dying on a log.
check     "a stale build cache is cleared and retried, not fatal" \
          grep -q "the existing build directory is stale" "$HERE/../install.sh"
check     "and the retry runs the same configure as the first attempt" \
          test "$(grep -c 'run_configure' "$HERE/../install.sh")" -ge 3

check_not "the installer no longer probes for symlink support" \
          grep -q "symlink-probe" "$HERE/../install.sh"
check_not "and never relocates the build tree" \
          grep -q "no symlinks; building in" "$HERE/../install.sh"
check     "the unversioning helper exists" \
          test -f "$HERE/../cmake/CrucibleUnversion.cmake"
check     "the dependency setup includes it" \
          grep -q "CrucibleUnversion.cmake" "$HERE/../cmake/CrucibleDependencies.cmake"
check     "and sweeps llama.cpp's targets once they exist" \
          grep -q 'crucible_unversion_directory("\${llama_SOURCE_DIR}")' \
               "$HERE/../cmake/CrucibleDependencies.cmake"
check_not "configuring without symlinks is no longer a fatal error" \
          grep -q "CRUCIBLE_FS_HAS_SYMLINKS" "$HERE/../cmake/CrucibleDependencies.cmake"
# The in-app runtime builder runs cmake on llama.cpp directly, with no Crucible
# CMakeLists in the picture, so it has to inject the same thing itself.
check     "the runtime builder injects the same hook" \
          grep -q "CMAKE_PROJECT_INCLUDE" "$HERE/../src/runtime/builder.cpp"
check     "and defines the sweep it points at" \
          grep -q "crucible_unversion_directory" "$HERE/../src/runtime/builder.cpp"

echo
echo "  llama tag"
# A runtime built from a different llama.cpp tag would load and then crash on
# the first tensor, so the installer's tag must match the one CMake pins.
CMAKE_TAG="$(grep -oE 'CRUCIBLE_LLAMA_TAG b[0-9]+' "$HERE/../cmake/CrucibleDependencies.cmake" | awk '{print $2}')"
check_eq  "install.sh pins the tag CMake pins" "$LLAMA_TAG" "$CMAKE_TAG"

# The same tag is compiled into the binary for the in-app runtime builder.
check     "CMakeLists passes the tag to the compiler" \
          grep -q 'CRUCIBLE_LLAMA_TAG="\${CRUCIBLE_LLAMA_TAG}"' "$HERE/../CMakeLists.txt"

# --------------------------------------------------------------------------
# Overall progress
#
# The five parts are nowhere near equal in length, so the main bar weights
# them. A bar that moved a fifth per part would read 80% with the entire build
# still ahead of it, which is worse than showing nothing.
# --------------------------------------------------------------------------
echo
echo "  overall_percent"

check_eq  "weights cover exactly the whole install" \
          "$(( ${STEP_WEIGHTS[1]} + ${STEP_WEIGHTS[2]} + ${STEP_WEIGHTS[3]} \
              + ${STEP_WEIGHTS[4]} + ${STEP_WEIGHTS[5]} ))" "100"
check_eq  "one weight per part, plus the unused zeroth" \
          "${#STEP_WEIGHTS[@]}" "$((STEP_TOTAL + 1))"

# The overall figure is the weighted sum of the finished parts plus however
# far STEP_PCT says the current one has got.
BEFORE_5=$(( ${STEP_WEIGHTS[1]} + ${STEP_WEIGHTS[2]} + ${STEP_WEIGHTS[3]} + ${STEP_WEIGHTS[4]} ))

STEP_PCT=0
STEP_NUM=0; check_eq "nothing done before the first part" "$(overall_percent)" "0"
STEP_NUM=1; check_eq "entering part 1"                    "$(overall_percent)" "0"
STEP_NUM=2; check_eq "entering part 2"                    "$(overall_percent)" "${STEP_WEIGHTS[1]}"
STEP_NUM=5; check_eq "entering part 5"                    "$(overall_percent)" "$BEFORE_5"
STEP_PCT=50
check_eq  "part 5 half done" "$(overall_percent)" "$((BEFORE_5 + STEP_WEIGHTS[5] / 2))"
STEP_PCT=100
check_eq  "part 5 complete"  "$(overall_percent)" "100"

# The build is the part worth weighting for: it must not read as nearly done.
STEP_NUM=5; STEP_PCT=0
check     "the main bar is under half way when the build starts" \
          test "$(overall_percent)" -lt 50

# Past the last part -- what the closing 100% bar sets.
STEP_NUM=$((STEP_TOTAL + 1)); STEP_PCT=0
check_eq  "past the end stays at 100"                     "$(overall_percent)" "100"

# Monotonic: the figure must never go backwards as a part progresses.
STEP_NUM=4
PREV=-1
MONOTONIC=1
for f in 0 10 25 50 75 90 100; do
    STEP_PCT="$f"
    CUR="$(overall_percent)"
    [ "$CUR" -lt "$PREV" ] && MONOTONIC=0
    PREV="$CUR"
done
check_eq  "progress within a part never goes backwards" "$MONOTONIC" "1"
STEP_NUM=0; STEP_PCT=0

# --------------------------------------------------------------------------
# Phases
#
# Each part is divided into phases that own a slice of it, so the per-part bar
# moves through the whole 0-100 whatever the part is doing. The two bars are
# the point of the display: one for the part, one for the install.
# --------------------------------------------------------------------------
echo
echo "  phases"

IS_TTY=0
STEP_NUM=2; STEP_PCT=0

# block_draw reports to stdout when there is no terminal, which is the right
# thing during a real install and only noise here.
phase 40 "first" >/dev/null
check_eq  "a phase starts where the part had got to"  "$STEP_PCT" "0"
phase_at 50 >/dev/null
check_eq  "half of a 40%-wide phase is 20% of the part" "$STEP_PCT" "20"
phase_at 100 >/dev/null
check_eq  "a full phase reaches its own end"           "$STEP_PCT" "40"
phase_end >/dev/null
check_eq  "ending a phase lands on its end exactly"    "$STEP_PCT" "40"

phase 60 "second" >/dev/null
check_eq  "the next phase starts where the last ended" "$PHASE_BASE" "40"
phase_at 50 >/dev/null
check_eq  "and is measured from there"                 "$STEP_PCT" "70"
phase_end >/dev/null
check_eq  "the phases of a part add up to all of it"   "$STEP_PCT" "100"

# cmake restarts its percentage for every target it builds, so a naive
# mapping would send the bar backwards several times during one compile.
STEP_PCT=0
phase 100 "third" >/dev/null
phase_at 90 >/dev/null
phase_at 10 >/dev/null
check_eq  "the bar never retreats"                     "$STEP_PCT" "90"

# A part whose phases add up to more than itself must still stop at 100.
STEP_PCT=0
phase 200 "overrun" >/dev/null
phase_at 100 >/dev/null
check_eq  "a part never reports more than complete"    "$STEP_PCT" "100"

STEP_NUM=0; STEP_PCT=0; PHASE_BASE=0; PHASE_SPAN=0

# --------------------------------------------------------------------------
# Install component
#
# llama.cpp and ggml install themselves into <prefix>/lib unconditionally,
# which for a system-wide install means a libllama.so that can shadow someone
# else's -- and files crucible --uninstall would leave behind. Installing only
# Crucible's own component is what stops that, so the flag is pinned here.
# --------------------------------------------------------------------------
echo
echo "  install component"

check     "CMakeLists puts Crucible's install rules in a component" \
          grep -q "COMPONENT \${CRUCIBLE_INSTALL_COMPONENT}" "$HERE/../CMakeLists.txt"
# Counting install(TARGETS) against COMPONENT used to stand in for this, and
# it is what let the bug through: the counts matched while the component was
# attached to the wrong artifact kind. What has to hold is that *every*
# artifact group in a rule names a component of its own, because options after
# LIBRARY or RUNTIME bind to that kind alone.
every_artifact_group_names_a_component() {
    awk '
        /install\(TARGETS/ { inrule = 1; group = 0; has = 0 }
        inrule {
            if ($0 ~ /(ARCHIVE|LIBRARY|RUNTIME|OBJECTS|FRAMEWORK|BUNDLE)[ \t]+DESTINATION/) {
                if (group && !has) bad = 1
                group = 1; has = 0
            }
            if ($0 ~ /COMPONENT/) has = 1
            if ($0 ~ /\)[ \t]*$/) {
                if (group && !has) bad = 1
                inrule = 0
            }
        }
        END { exit bad ? 1 : 0 }
    ' "$1"
}
check     "every artifact group in an install rule names its component" \
          every_artifact_group_names_a_component "$HERE/../CMakeLists.txt"
check     "the installer asks for that component" \
          grep -q -- "--install .* --component crucible" "$HERE/../install.sh"
check_eq  "every install invocation is scoped" \
          "$(grep -c 'CMAKE" --install' "$HERE/../install.sh")" \
          "$(grep -c -- '--install .* --component crucible' "$HERE/../install.sh")"
# GGML_BACKEND_DIR would bake an absolute search path into the binary and put
# the runtimes outside the component; the path is passed at startup instead.
check_not "GGML_BACKEND_DIR is not set" \
          grep -q "^ *set(GGML_BACKEND_DIR" "$HERE/../cmake/CrucibleDependencies.cmake"

echo
echo "  bars"
IS_TTY=0
check     "a bar renders its percentage" \
          grep -q "100%" <<< "$(draw_bar 100 "")"
check     "an out-of-range percentage is clamped, not drawn off the end" \
          grep -q "100%" <<< "$(draw_bar 140 "")"
check     "a negative percentage clamps to zero" \
          grep -q "0%" <<< "$(draw_bar -20 "")"

# One bar, for the whole install. There used to be a second bar underneath for
# the part running now; it is gone, and these pin that it stays gone -- two
# progress figures on screen at once is two numbers to reconcile, and the part
# being worked on is already named twice (in the ==> heading and in the label
# beside the bar).
IS_TTY=1
PROGRESS_ON=1
STEP_NUM=3; STEP_PCT=50; PHASE_LABEL="probe"; PHASE_START=0
BLOCK="$(block_draw)"
check     "the bar names the whole install"   grep -q "install" <<< "$BLOCK"
check     "the bar shows the phase label"     grep -q "probe"   <<< "$BLOCK"
check_eq  "the bar is exactly one row"        "$(printf '%s' "$BLOCK" | grep -c '')" "1"
check_not "the bar carries no second percentage" \
          grep -q "%.*%" <<< "$BLOCK"
check_not "the step counter is left to the heading above" \
          grep -q "\[3/5\]" <<< "$BLOCK"

# The elapsed clock. A phase that starts in the very first second has
# PHASE_START=0, and zero is a real start time -- it must not be mistaken for
# the -1 that means "no phase is running".
#
# What is asserted is that a clock appears at all, not what it reads. The
# elapsed figure is SECONDS - PHASE_START, and with PHASE_START=0 that is
# however long this script has been running: pinning it to "(0s)" made the
# check pass or fail on whether the machine was busy enough for the suite to
# take a second to get here.
STEP_NUM=3; STEP_PCT=0; PHASE_LABEL="prompt"
PHASE_START=0
check     "a phase that started in the first second still shows its clock" \
          grep -q "s)" <<< "$(block_draw)"
PHASE_START=-1
check_not "a part with no phase running shows no clock" \
          grep -q "s)" <<< "$(block_draw)"

# The bar goes up before the first part rather than with it, so it is on screen
# from the start of the install instead of appearing a few seconds in.
PROGRESS_ON=1; STEP_NUM=0; STEP_PCT=0; PHASE_LABEL="starting"
check     "the bar is drawn before the first part begins" \
          grep -q "install" <<< "$(block_draw)"

# ...but only once an install is actually under way, which is what keeps it out
# of --check, the banner and the uninstaller. All of those print through note(),
# which draws the block after every line.
PROGRESS_ON=0
check_eq  "nothing is drawn when no install is running" "$(block_draw)" ""

# Retiring it leaves one finished bar in the scrollback and stops the redraws,
# so the closing summary -- which is written with plain printf and knows nothing
# about the cursor arithmetic -- cannot land inside the block.
PROGRESS_ON=1; STEP_NUM=2; STEP_PCT=10; BLOCK_SHOWN=0
# Redirected rather than captured with $(...): a command substitution runs in a
# subshell, so the PROGRESS_ON=0 that retirement depends on would be discarded
# along with it and the test would be checking nothing.
progress_end > "$TMP/retired"
check     "retiring the bar leaves it at 100%" grep -q "100%" "$TMP/retired"
check_eq  "and stops it redrawing"             "$PROGRESS_ON" "0"
check_eq  "and nothing is drawn afterwards"    "$(block_draw)" ""

IS_TTY=0
PROGRESS_ON=0
STEP_NUM=0; STEP_PCT=0

# --------------------------------------------------------------------------
# Runtimes
#
# Crucible installs with no compute backend at all -- not even CPU. The runtimes
# directory is created empty and the settings screen fills it, which is what
# makes the choice of backend reversible.
# --------------------------------------------------------------------------
echo
echo "  the one-line install builds both faces"

# Crucible is two faces over one engine, so the one-line install gives you
# both. The GUI is still the only part that needs anything from the system
# beyond a compiler, which is what the fallback below is for.
check     "the installer builds the desktop app by default" \
          grep -q '^WITH_GUI=1$' "$HERE/../install.sh"
check     "--no-gui opts out" \
          grep -q -- '--no-gui)    WITH_GUI=0' "$HERE/../install.sh"
check     "--gui is still accepted, and still means yes" \
          grep -q -- '--gui)       WITH_GUI=1' "$HERE/../install.sh"
check     "the installer passes the choice through to cmake" \
          grep -q 'DCRUCIBLE_BUILD_GUI=' "$HERE/../install.sh"

# The CMake option stays off. The installers can add the OpenGL and X11 headers
# first and step down to the terminal program when they cannot; a bare `cmake`
# run can do neither, so defaulting it on there would turn a missing system
# header into a failed build.
check     "the CMake option itself stays off for a bare build" \
          grep -q 'option(CRUCIBLE_BUILD_GUI .* OFF)' "$HERE/../CMakeLists.txt"

check     "the GUI's packages are behind the flag" \
          grep -q 'WITH_GUI" = "1" \] && \[ "${#PKGS_GUI\[@\]}"' "$HERE/../install.sh"
# This is what makes the new default safe: a machine with no window library
# gets the terminal program and a warning, not a failed install.
check     "a missing window library falls back to the terminal app" \
          grep -q 'building the terminal app only' "$HERE/../install.sh"
check     "the fallback leaves WITH_GUI off, so the summary tells the truth" \
          grep -q 'WITH_GUI=0$' "$HERE/../install.sh"

# Windows: switches default to false, so the opt-out is the switch and the
# build flag has to be derived from it rather than read directly.
check     "Windows takes -NoGui" \
          grep -q '\[switch\] \$NoGui' "$HERE/../install.ps1"
check     "Windows derives the build flag from it" \
          grep -q 'BuildGui = -not \$NoGui' "$HERE/../install.ps1"
check_not "Windows no longer gates the build on the bare -Gui switch" \
          grep -q 'CRUCIBLE_BUILD_GUI="\$(if (\$Gui)' "$HERE/../install.ps1"

echo
echo "  no runtimes are installed"

check_not "the installer builds no runtime" \
          grep -q "prebuild_runtime" "$HERE/../install.sh"
check_not "no backend modules are installed by CMake" \
          grep -q "GGML_AVAILABLE_BACKENDS" "$HERE/../CMakeLists.txt"
check     "the CPU backend is not compiled into the build" \
          grep -q "set(GGML_CPU              OFF CACHE INTERNAL" \
          "$HERE/../cmake/CrucibleDependencies.cmake"
# ggml sets GGML_METAL_DEFAULT and GGML_BLAS_DEFAULT to ON under APPLE, so
# "no backend at all" is only true on a Mac if all three are named explicitly.
for opt in GGML_METAL GGML_BLAS GGML_ACCELERATE; do
    check "the base build turns $opt off (ggml defaults it on for Apple)" \
          grep -q "set($opt *OFF CACHE INTERNAL" \
          "$HERE/../cmake/CrucibleDependencies.cmake"
done

# Same defaults, same problem, one layer down: a CPU runtime built on a Mac
# would otherwise emit ggml-metal and ggml-blas alongside it.
# The desktop app's typeface is compiled in, not looked for. A missing font must
# downgrade to a system face rather than fail the build.
check     "the interface font is fetched and pinned" \
          grep -q 'CRUCIBLE_FONT_TAG' "$HERE/../cmake/CrucibleDependencies.cmake"
check     "a font that will not fetch does not break the build" \
          grep -q 'CRUCIBLE_FONTS_EMBEDDED OFF' \
          "$HERE/../cmake/CrucibleDependencies.cmake"
check     "the app knows whether it has one" \
          grep -q 'CRUCIBLE_HAS_EMBEDDED_FONT' \
          "$HERE/../cmake/CrucibleDependencies.cmake"

echo
echo "  Windows"

# The Windows installer is a separate script, because PowerShell is what is
# there and bash is not. It has to offer the same shape of thing.
check     "there is a Windows installer" \
          test -f "$HERE/../install.ps1"
check     "it takes the same options the shell one does" \
          grep -q 'param(' "$HERE/../install.ps1"
for opt in Gpu Prefix Gui Uninstall Check; do
    check "  -$opt" grep -q "\\\$$opt" "$HERE/../install.ps1"
done
# It must not build a runtime either: that decision is the same on every
# platform, and it belongs to the settings screen.
check_not "it does not build a GPU runtime" \
          grep -q 'CRUCIBLE_BUILD_RUNTIME\|prebuild_runtime' "$HERE/../install.ps1"
check     "it installs only Crucible's own component" \
          grep -q -- '--component crucible' "$HERE/../install.ps1"
# RPATH is an ELF idea. On Windows the loader looks beside the executable, so
# the libraries have to be installed there instead.
check     "Windows installs the libraries beside the binary" \
          grep -q 'RUNTIME DESTINATION \${CMAKE_INSTALL_BINDIR}' "$HERE/../CMakeLists.txt"
check     "and does not try to bake in an RPATH" \
          grep -q 'CRUCIBLE_BACKEND_DL AND NOT WIN32' "$HERE/../CMakeLists.txt"
# MSVC assumes the system code page without this and mangles every non-ASCII
# glyph the sprite and the expert panel draw with.
check     "MSVC is told the sources are UTF-8" \
          grep -q 'add_compile_options(/utf-8)' "$HERE/../CMakeLists.txt"

echo
echo "  the built-in fallback seat is gone"

# It was a tenth expert the delegator could never name. Any ordinary seat can
# play that part now, and routing.default_expert says which.
check_not "no expert is special-cased as a fallback" \
          grep -rq 'kFallbackId' "$HERE/../src" "$HERE/../include"
check_not "no seat is marked unroutable" \
          grep -rq 'routable' "$HERE/../src" "$HERE/../include"
check     "the nominated default is what catches the rest" \
          grep -q 'default_expert' "$HERE/../include/crucible/config/config.hpp"

echo
echo "  a cook can change hands"

# The verb table specifically, not the instructions -- those name it too, so a
# grep for the word alone would pass with the verb renamed out from under it.
check     "HANDOFF is in the verb table" \
          grep -q '"HANDOFF", ToolKind::Handoff' "$HERE/../src/tools/workshop.cpp"
check     "the cook loop re-routes on one" \
          grep -q 'take_the_seat' "$HERE/../src/engine/engine_cook.cpp"
# The whole memory argument for the design: one expert resident at a time.
check     "the previous expert is freed before the next is loaded" \
          grep -q 'acquire_expert' "$HERE/../src/engine/engine_cook.cpp"

echo
echo "  no runtimes are installed (continued)"

check     "a runtime build turns off BLAS and Accelerate" \
          grep -q -- '-DGGML_BLAS=OFF' "$HERE/../src/runtime/builder.cpp"
check     "a runtime build turns off Metal unless Metal is what was asked for" \
          grep -q "kind != BackendKind::Metal" "$HERE/../src/runtime/builder.cpp"

check     "the llama.cpp source is still kept, so a later build needs no network" \
          grep -q "seed_runtime_source" "$HERE/../install.sh"
check     "the summary says no runtime is installed" \
          grep -q "runtimes : .*none yet" "$HERE/../install.sh"

echo "  failure reporting"

# The report Matt got from the Mac was 25 lines of successful status messages
# and no error, because the tail of a log is not where the error necessarily
# is. A failure report that omits the failure costs a whole round trip.
buried_log="$(mktemp)"; buried_out="$(mktemp)"
{
    echo "CMake Error at ggml/src/CMakeLists.txt:592 (add_subdirectory):"
    echo "  The source directory does not contain a CMakeLists.txt file."
    i=1
    while [ "$i" -le 30 ]; do echo "-- harmless status line $i"; i=$((i + 1)); done
} > "$buried_log"
show_log_tail "$buried_log" 25 > "$buried_out" 2>&1 || true

check     "an error buried above the tail is still reported" \
          grep -q "CMake Error" "$buried_out"
check     "the tail is shown as well, not instead" \
          grep -q "harmless status line 30" "$buried_out"
rm -f "$buried_log" "$buried_out"

echo "  bash 3.2 on macOS"

# macOS ships bash 3.2.57 and always will -- Apple froze it in 2007 over the
# GPLv3 -- so `#!/usr/bin/env bash` finds 3.2 on a machine that has not
# installed a newer one, which is every machine this script is run on.
#
# 3.2 scans a bare $name with isalnum() a byte at a time, and macOS's ctype
# table answers yes for the bytes of a UTF-8 character. So a bare colour
# variable written against a tick becomes a variable whose name has the tick's
# three bytes glued on the end, which is unset, and `set -u` kills the install --
# which is exactly what happened at step 2/5, reported as
# "C_GRN?: unbound variable" because the terminal cannot print the bytes back.
#
# Linux never shows this: glibc's isalnum says no for those bytes, so the same
# line is correct here and fatal there. Braces are the fix, and this is the
# check that keeps them.
# grep -P is GNU-only, and this check has to run on the machine it is about.
# LC_ALL=C makes the bracket a byte range rather than a character one, which is
# the whole point: it is the raw bytes 3.2 misreads.
check_not "no bare \$VAR is followed by a non-ASCII character (bash 3.2 eats it)" \
          env LC_ALL=C grep -qE '[$][A-Za-z_][A-Za-z0-9_]*[^ -~'"$(printf '\t')"']' \
          "$HERE/../install.sh" "$HERE/../tests/test_install.sh" \
          "$HERE/../tests/test_uninstall.sh"

# The 4.x features that would fail the same way: silently on Linux, fatally on
# a Mac. Associative arrays and the upper/lower-casing expansions are the ones
# most likely to be reached for here.
check_not "no bash 4+ syntax the Mac's shell cannot parse" \
          grep -qE '(declare|local) -[A]|(declare|local) -[n]|map[f]ile|read[a]rray|[$][{][A-Za-z_][A-Za-z0-9_]*(\^\^|,,)' \
          "$HERE/../install.sh" "$HERE/../tests/test_install.sh" \
          "$HERE/../tests/test_uninstall.sh"

# An empty array expanded as "${arr[@]}" is an unbound variable in 3.2 -- so
# the Mac, where PKGS_VULKAN and PKGS_CUDA are both empty, is the one machine
# where it fires. ${arr[@]+"${arr[@]}"} is the portable spelling.
check_not "no bare \"\${arr[@]}\" expansion (empty arrays are fatal in bash 3.2)" \
          grep -qE '[^+]"[$][{]PKGS_[A-Z]*\[@\][}]"' "$HERE/../install.sh"

runtime_for() {
    ( OS="$1"; RUNTIME="$2"; decide_backend >/dev/null 2>&1; printf '%s' "$RUNTIME" )
}

check_eq "a Mac asked for auto gets Metal" "$(runtime_for Darwin auto)" "metal"
check_eq "a Mac may ask for Metal outright" "$(runtime_for Darwin metal)" "metal"
# die exits non-zero, so the subshell prints nothing at all.
check_eq "CUDA on a Mac is refused, not attempted" "$(runtime_for Darwin cuda)" ""
check_eq "Metal off a Mac is refused, not attempted" "$(runtime_for Linux metal)" ""
check     "the refusal names what to use instead" \
          grep -q -- "use --gpu metal (or --gpu cpu)" "$HERE/../install.sh"

echo
echo "  the crucible component carries the libraries the binary links against"

# The bug this pins down: in install(TARGETS), every option after LIBRARY or
# RUNTIME binds to that artifact kind alone. A single trailing COMPONENT
# attaches to RUNTIME and leaves the LIBRARY rule in CMake's default
# "Unspecified" component, which `--install --component crucible` never
# installs. The binaries land, the shared objects they need do not, and
# crucible dies on startup with
#   "libllama.so: cannot open shared object file: No such file or directory".
#
# Read off the generated install script rather than by installing: it names the
# component of every file, needs no build, and is what CMake will actually run.
filed_under_crucible() {
    awk -v want="$2" '
        /^if\(CMAKE_INSTALL_COMPONENT STREQUAL "/ {
            component = $0
            sub(/.*STREQUAL "/, "", component)
            sub(/".*/, "", component)
        }
        index($0, want) { seen = 1; if (component != "crucible") wrong = 1 }
        END { exit (seen && !wrong) ? 0 : 1 }
    ' "$1"
}

GENERATED="$HERE/../build/cmake_install.cmake"
if [ -f "$GENERATED" ]; then
    for _lib in libllama.so libggml.so libggml-base.so; do
        check "$_lib is filed under the crucible component" \
              filed_under_crucible "$GENERATED" "lib/crucible/$_lib"
    done
    check "the crucible binary is filed under the crucible component" \
          filed_under_crucible "$GENERATED" "bin/crucible"
else
    echo "    (skipped: no configured build tree at build/)"
fi

echo
echo "$((PASS + FAIL)) checks, $FAIL failed"
[ "$FAIL" -eq 0 ]
