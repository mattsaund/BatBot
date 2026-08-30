#!/usr/bin/env bash
# Safety tests for `batbot --uninstall`.
#
# Uninstall is the only destructive thing BatBot does, and a clean reinstall
# test depends on it actually being clean: yes to everything must leave nothing
# behind, and each answer must be independent. Pinned down by running the real
# binary against a sandboxed HOME rather than by reading the code.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BATBOT="${1:-$HERE/../build/bin/batbot}"

if [ ! -x "$BATBOT" ]; then
    echo "batbot binary not found at $BATBOT -- build first" >&2
    exit 2
fi

# These tests run the real binary, so a stale one reports failures that look
# like product bugs and are not. Refuse rather than mislead.
# The parentheses matter: without them find reads this as
# "(-name '*.cpp') OR ('*.hpp' AND -newer)", which matches every source file
# whatever its age.
NEWER="$(find "$HERE/../src" "$HERE/../include" \
         \( -name '*.cpp' -o -name '*.hpp' \) -newer "$BATBOT" 2>/dev/null | head -1)"
if [ -n "$NEWER" ]; then
    echo "$BATBOT is older than $NEWER -- rebuild, or pass the right binary" >&2
    exit 2
fi

PASS=0
FAIL=0
check() {
    local what="$1" got="$2" want="$3"
    if [ "$got" = "$want" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '    FAIL: %s\n         got: %s\n    expected: %s\n' "$what" "$got" "$want"
    fi
}

SANDBOX="$(mktemp -d)"
trap 'rm -rf "$SANDBOX"' EXIT

# Build a throwaway install: a copy of the binary, a config, and two "models".
setup() {
    local root="$SANDBOX/$1"
    rm -rf "$root"
    mkdir -p "$root/bin" "$root/cfg/batbot" "$root/dat/batbot/models"
    # The bootstrapped CMake and, on a filesystem without symlinks, the whole
    # build tree live in the cache. Uninstall removes it -- which is exactly
    # why this has to be sandboxed: without XDG_CACHE_HOME below, running
    # these tests deletes the developer's own build directory.
    mkdir -p "$root/cache/batbot/build"
    head -c 65536 /dev/zero > "$root/cache/batbot/build/CMakeCache.txt"
    # The installed layout: the binary in bin/ and llama.cpp's shared
    # libraries in lib/batbot/. The binary is not the whole program any more,
    # and uninstall has to know that.
    #
    # These have to be the real libraries, not stand-ins. A loadable build
    # resolves libllama.so through an RPATH of $ORIGIN/../lib/batbot, so a
    # sandbox holding a zero-filled file of that name gives a binary that
    # cannot start -- and every check then reads "nothing was removed", which
    # looks exactly like an uninstaller that does nothing.
    mkdir -p "$root/lib/batbot/runtimes"
    local built=0
    for lib in "$(dirname "$BATBOT")"/lib*.so; do
        [ -e "$lib" ] || continue
        cp "$lib" "$root/lib/batbot/"
        built=1
    done
    if [ "$built" = 0 ]; then
        # A monolithic build has no shared libraries at all; the file only has
        # to exist for the "libraries removed" check to mean something.
        head -c 262144 /dev/zero > "$root/lib/batbot/libllama.so"
    fi
    head -c 262144 /dev/zero > "$root/lib/batbot/runtimes/libggml-cpu-haswell.so"
    # A GPU runtime the user built, plus the source and build tree behind it.
    mkdir -p "$root/dat/batbot/runtimes" "$root/dat/batbot/runtime-src" \
             "$root/dat/batbot/runtime-build/vulkan" "$root/dat/batbot/projects/demo-abc12345"
    head -c 524288 /dev/zero > "$root/dat/batbot/runtimes/libggml-vulkan.so"
    echo '{"vulkan":{"llama_tag":"b10678"}}' > "$root/dat/batbot/runtimes/manifest.json"
    echo '{"turns":[]}' > "$root/dat/batbot/projects/demo-abc12345/usage.json"
    cp "$BATBOT" "$root/bin/batbot"
    # The installer puts this beside the binary, so uninstall has to take it.
    printf '#!/bin/sh\n' > "$root/bin/batbot-routebench"; chmod +x "$root/bin/batbot-routebench"
    echo '{"models_dir":""}' > "$root/cfg/batbot/config.json"
    echo '{"trusted":[]}'    > "$root/cfg/batbot/trust.json"
    head -c 1048576 /dev/zero > "$root/dat/batbot/models/expensive-expert.gguf"
    head -c 1048576 /dev/zero > "$root/dat/batbot/models/another-expert.gguf"
    printf '%s' "$root"
}

run_uninstall() {  # root, answers, extra args
    local root="$1" answers="$2"; shift 2
    printf '%b' "$answers" | XDG_CONFIG_HOME="$root/cfg" XDG_DATA_HOME="$root/dat" \
        XDG_CACHE_HOME="$root/cache" \
        "$root/bin/batbot" --uninstall "$@" >/dev/null 2>&1
}

count_models() { ls "$1/dat/batbot/models" 2>/dev/null | wc -l | tr -d ' '; }
exists()       { [ -e "$1" ] && echo yes || echo no; }

echo "batbot --uninstall safety tests"
echo

echo "  declining leaves everything in place"
ROOT="$(setup declining)"
run_uninstall "$ROOT" 'n\nn\nn\n'
check "binary kept"           "$(exists "$ROOT/bin/batbot")"            "yes"
check "config kept"           "$(exists "$ROOT/cfg/batbot/config.json")" "yes"
check "models kept"           "$(count_models "$ROOT")"                  "2"

echo "  yes to everything leaves nothing behind"
# What a clean reinstall test needs: one pass, nothing left to tidy up by hand.
ROOT="$(setup everything)"
run_uninstall "$ROOT" 'y\ny\ny\n'
check "binary removed"        "$(exists "$ROOT/bin/batbot")"            "no"
check "config removed"        "$(exists "$ROOT/cfg/batbot")"            "no"
check "data removed"          "$(exists "$ROOT/dat/batbot")"            "no"
# The shared libraries are most of the install by size; leaving them behind
# would make "yes to everything" a lie.
check "libraries removed"     "$(exists "$ROOT/lib/batbot")"            "no"
check "routebench removed"    "$(exists "$ROOT/bin/batbot-routebench")"  "no"
check "user runtimes removed" "$(exists "$ROOT/dat/batbot/runtimes")"   "no"
check "runtime source removed" "$(exists "$ROOT/dat/batbot/runtime-src")" "no"
check "project history removed" "$(exists "$ROOT/dat/batbot/projects")" "no"
check "cache removed"         "$(exists "$ROOT/cache/batbot")"          "no"

echo "  -y answers yes to every question"
ROOT="$(setup assume_yes)"
run_uninstall "$ROOT" '' -y
check "binary removed"        "$(exists "$ROOT/bin/batbot")"            "no"
check "config removed"        "$(exists "$ROOT/cfg/batbot")"            "no"
check "data removed"          "$(exists "$ROOT/dat/batbot")"            "no"
# The shared libraries are most of the install by size; leaving them behind
# would make "yes to everything" a lie.
check "libraries removed"     "$(exists "$ROOT/lib/batbot")"            "no"
check "routebench removed"    "$(exists "$ROOT/bin/batbot-routebench")"  "no"
check "user runtimes removed" "$(exists "$ROOT/dat/batbot/runtimes")"   "no"
check "runtime source removed" "$(exists "$ROOT/dat/batbot/runtime-src")" "no"
check "project history removed" "$(exists "$ROOT/dat/batbot/projects")" "no"
check "cache removed"         "$(exists "$ROOT/cache/batbot")"          "no"

echo "  each answer is independent: keep the config, drop the rest"
ROOT="$(setup partial)"
run_uninstall "$ROOT" 'y\nn\ny\n'
check "binary removed"        "$(exists "$ROOT/bin/batbot")"            "no"
check "config kept"           "$(exists "$ROOT/cfg/batbot/config.json")" "yes"
check "data removed"          "$(exists "$ROOT/dat/batbot")"            "no"

echo "  keeping the binary is possible"
ROOT="$(setup keep_binary)"
run_uninstall "$ROOT" 'n\nn\nn\n'
check "binary kept"           "$(exists "$ROOT/bin/batbot")"            "yes"
check "models kept"           "$(count_models "$ROOT")"                  "2"

echo "  a bare install with nothing to remove does not fail"
ROOT="$SANDBOX/bare"
mkdir -p "$ROOT/bin" "$ROOT/cfg" "$ROOT/dat"
cp "$BATBOT" "$ROOT/bin/batbot"
# XDG_CACHE_HOME as well, for the same reason as run_uninstall above: the
# uninstaller removes the build cache, and an unsandboxed run of this test
# would remove the real one belonging to whoever is running the suite.
printf 'n\n' | XDG_CONFIG_HOME="$ROOT/cfg" XDG_DATA_HOME="$ROOT/dat" \
    XDG_CACHE_HOME="$ROOT/cache" \
    "$ROOT/bin/batbot" --uninstall >/dev/null 2>&1
check "exit code"             "$?"                                       "0"

echo
echo "$((PASS + FAIL)) checks, $FAIL failed"
[ "$FAIL" -eq 0 ]
