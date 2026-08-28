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

echo
echo "$((PASS + FAIL)) checks, $FAIL failed"
[ "$FAIL" -eq 0 ]
