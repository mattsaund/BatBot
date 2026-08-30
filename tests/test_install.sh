#!/usr/bin/env bash
# Tests for install.sh's pure logic: version comparison, the CUDA-vs-hardware
# table, and CMake version detection. Getting these wrong means a GPU that
# silently cannot be used, so they are worth pinning down.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BATBOT_INSTALL_LIB=1
export BATBOT_INSTALL_LIB
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
check_eq  "an ordinary filesystem builds in place" "$BUILD_DIR" "$SRC_DIR/build"

echo
echo "  llama tag"
# A runtime built from a different llama.cpp tag would load and then crash on
# the first tensor, so the installer's tag must match the one CMake pins.
CMAKE_TAG="$(grep -oE 'BATBOT_LLAMA_TAG b[0-9]+' "$HERE/../cmake/BatBotDependencies.cmake" | awk '{print $2}')"
check_eq  "install.sh pins the tag CMake pins" "$LLAMA_TAG" "$CMAKE_TAG"

# The same tag is compiled into the binary for the in-app runtime builder.
check     "CMakeLists passes the tag to the compiler" \
          grep -q 'BATBOT_LLAMA_TAG="\${BATBOT_LLAMA_TAG}"' "$HERE/../CMakeLists.txt"

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
# else's -- and files batbot --uninstall would leave behind. Installing only
# BatBot's own component is what stops that, so the flag is pinned here.
# --------------------------------------------------------------------------
echo
echo "  install component"

check     "CMakeLists puts BatBot's install rules in a component" \
          grep -q "COMPONENT \${BATBOT_INSTALL_COMPONENT}" "$HERE/../CMakeLists.txt"
check     "no install rule is left outside that component" \
          test "$(grep -c '^ *install(TARGETS' "$HERE/../CMakeLists.txt")" \
               = "$(grep -c 'COMPONENT \${BATBOT_INSTALL_COMPONENT}' "$HERE/../CMakeLists.txt")"
check     "the installer asks for that component" \
          grep -q -- "--install .* --component batbot" "$HERE/../install.sh"
check_eq  "every install invocation is scoped" \
          "$(grep -c 'CMAKE" --install' "$HERE/../install.sh")" \
          "$(grep -c -- '--install .* --component batbot' "$HERE/../install.sh")"
# GGML_BACKEND_DIR would bake an absolute search path into the binary and put
# the runtimes outside the component; the path is passed at startup instead.
check_not "GGML_BACKEND_DIR is not set" \
          grep -q "^ *set(GGML_BACKEND_DIR" "$HERE/../cmake/BatBotDependencies.cmake"

echo
echo "  bars"
IS_TTY=0
check     "a bar renders its percentage" \
          grep -q "100%" <<< "$(draw_bar 100 "")"
check     "an out-of-range percentage is clamped, not drawn off the end" \
          grep -q "100%" <<< "$(draw_bar 140 "")"
check     "a negative percentage clamps to zero" \
          grep -q "0%" <<< "$(draw_bar -20 "")"

# Both bars are drawn together, and the top one is labelled so the two are
# never mistaken for each other.
IS_TTY=1
STEP_NUM=3; STEP_PCT=50; PHASE_LABEL="probe"; PHASE_START=0
BLOCK="$(block_draw)"
check     "the block names the whole install"  grep -q "install" <<< "$BLOCK"
check     "the block names the part"           grep -q "\[3/5\]" <<< "$BLOCK"
check     "the block shows the phase label"    grep -q "probe"   <<< "$BLOCK"
check_eq  "the block is exactly two rows"      "$(printf '%s' "$BLOCK" | grep -c '')" "2"

# The elapsed clock. SECONDS is 0 for the whole first second of a run, so a
# phase that starts promptly must still be recognised as running.
STEP_NUM=3; STEP_PCT=0; PHASE_LABEL="prompt"
PHASE_START=0
check     "a phase that started in the first second still shows its clock" \
          grep -q "(0s)" <<< "$(block_draw)"
PHASE_START=-1
check_not "a part with no phase running shows no clock" \
          grep -q "s)" <<< "$(block_draw)"

# Before the first part there is no install to report on, which is what keeps
# the bars out of --check and out of the uninstaller.
STEP_NUM=0
check_eq  "nothing is drawn before the first part" "$(block_draw)" ""
IS_TTY=0
STEP_NUM=0; STEP_PCT=0

# --------------------------------------------------------------------------
# Runtimes
#
# BatBot installs with no compute backend at all -- not even CPU. The runtimes
# directory is created empty and the settings screen fills it, which is what
# makes the choice of backend reversible.
# --------------------------------------------------------------------------
echo
echo "  no runtimes are installed"

check_not "the installer builds no runtime" \
          grep -q "prebuild_runtime" "$HERE/../install.sh"
check_not "no backend modules are installed by CMake" \
          grep -q "GGML_AVAILABLE_BACKENDS" "$HERE/../CMakeLists.txt"
check     "the CPU backend is not compiled into the build" \
          grep -q "set(GGML_CPU              OFF CACHE INTERNAL" \
          "$HERE/../cmake/BatBotDependencies.cmake"
check     "the llama.cpp source is still kept, so a later build needs no network" \
          grep -q "seed_runtime_source" "$HERE/../install.sh"
check     "the summary says no runtime is installed" \
          grep -q "runtimes : .*none yet" "$HERE/../install.sh"

echo
echo "$((PASS + FAIL)) checks, $FAIL failed"
[ "$FAIL" -eq 0 ]
