#!/usr/bin/env bash
#
# BatBot installer.
#
#   curl -fsSL https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh | bash
#
# Installs the build toolchain and the GPU SDKs, builds BatBot with loadable
# runtimes, pre-builds the GPU runtime this machine wants, and puts the binary
# on your PATH. Safe to re-run: it upgrades in place.

set -euo pipefail

REPO_URL="https://github.com/mattsaund/batbot.git"
RAW_URL="https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh"

BRANCH="main"
RUNTIME="auto"

# Must match BATBOT_LLAMA_TAG in cmake/BatBotDependencies.cmake: a runtime
# built from a different tag would load and then crash on the first tensor.
LLAMA_TAG="b10678"
PREFIX=""
INSTALL_DEPS=1
ASSUME_YES=0
DO_UNINSTALL=0
DO_CHECK=0
JOBS="$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)"

CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=24
CMAKE_BOOTSTRAP_VERSION="3.31.6"

CMAKE="cmake"
SUDO=""
PKG=""
SRC_DIR=""
CLONED_FRESH=0

# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_YEL=$'\033[33m';  C_GRN=$'\033[32m'; C_RED=$'\033[31m'; C_CYN=$'\033[36m'
else
    C_RESET=""; C_BOLD=""; C_DIM=""; C_YEL=""; C_GRN=""; C_RED=""; C_CYN=""
fi

IS_TTY=0
[ -t 1 ] && IS_TTY=1

# Bash slices strings by character only in a multibyte locale, and the block
# glyphs are multibyte. Check before using them, or the bar draws at the wrong
# width under LC_ALL=C.
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *[Uu][Tt][Ff]*8*) BAR_FILL="█"; BAR_VOID="░" ;;
    *)                BAR_FILL="#"; BAR_VOID="-" ;;
esac

BAR_WIDTH=28
PROGRESS_LAST=-1

# Set while a step bar should also report where the whole install has got to.
# BASE and SPAN map that bar's own 0-100 onto a slice of the current part, so
# two long operations inside one part (building BatBot, then building a GPU
# runtime) advance the main figure instead of each restarting it.
OVERALL_TRACK=0
OVERALL_BASE=0
OVERALL_SPAN=100
SPINNER_PID=""

repeat_char() {
    local char="$1" count="$2" out=""
    while [ "$count" -gt 0 ]; do out="$out$char"; count=$((count - 1)); done
    printf '%s' "$out"
}

clear_line() { [ "$IS_TTY" = 1 ] && printf '\r\033[2K'; return 0; }

term_cols() {
    local cols=""
    if [ "$IS_TTY" = 1 ]; then
        cols="$(tput cols 2>/dev/null || true)"
    fi
    case "$cols" in ''|*[!0-9]*) cols="${COLUMNS:-80}" ;; esac
    case "$cols" in ''|*[!0-9]*) cols=80 ;; esac
    printf '%s' "$cols"
}

# Trim a label so the whole status line fits on one terminal row.
#
# A wrapped line is not cosmetic: clear_line only erases the row the cursor is
# on, so anything that spilled onto a second row is left behind as litter when
# the spinner is replaced by its result.
fit_label() {
    local label="$1" reserved="$2" width
    width=$(( $(term_cols) - reserved ))
    [ "$width" -lt 8 ] && width=8
    if [ "${#label}" -gt "$width" ]; then
        printf '%s…' "${label:0:$((width - 1))}"
    else
        printf '%s' "$label"
    fi
}

hide_cursor() { [ "$IS_TTY" = 1 ] && printf '\033[?25l'; return 0; }
show_cursor() { [ "$IS_TTY" = 1 ] && printf '\033[?25h'; return 0; }

# Draw a bar at `percent` with a trailing label. Redraws only when the
# percentage actually moves, so a chatty build does not flicker.
progress_render() {
    local percent="$1" label="${2:-}" filled empty
    [ "$percent" -lt 0 ] && percent=0
    [ "$percent" -gt 100 ] && percent=100

    # A long part would otherwise leave the main bar frozen for minutes, so the
    # step bar carries the live overall figure while it runs.
    if [ "$OVERALL_TRACK" = 1 ]; then
        label="install $(overall_percent $((OVERALL_BASE + percent * OVERALL_SPAN / 100)))%  ·  $label"
    fi

    if [ "$IS_TTY" != 1 ]; then
        # Not a terminal (CI, or output redirected to a file): emit a line at
        # each 20% instead of a bar, so logs stay readable.
        if [ "$((percent / 20))" -ne "$((PROGRESS_LAST / 20))" ] || [ "$PROGRESS_LAST" -lt 0 ]; then
            printf '    %3d%%  %s\n' "$percent" "$label"
            PROGRESS_LAST="$percent"
        fi
        return 0
    fi

    [ "$percent" = "$PROGRESS_LAST" ] && return 0
    PROGRESS_LAST="$percent"

    local bar_width="$BAR_WIDTH" cols
    cols="$(term_cols)"
    # On a narrow terminal the bar yields space before the label does.
    [ "$cols" -lt 70 ] && bar_width=14
    [ "$cols" -lt 50 ] && bar_width=8

    filled=$((percent * bar_width / 100))
    empty=$((bar_width - filled))
    # 4 indent + bar + " 100%" + 2 spaces
    label="$(fit_label "$label" $((bar_width + 13)))"

    printf '\r\033[2K    %s%s%s%s%s %3d%%  %s%s%s' \
        "$C_CYN" "$(repeat_char "$BAR_FILL" "$filled")" \
        "$C_DIM" "$(repeat_char "$BAR_VOID" "$empty")" "$C_RESET" \
        "$percent" "$C_DIM" "$label" "$C_RESET"
}

progress_begin() { PROGRESS_LAST=-1; hide_cursor; }

# Report overall progress from the next step bar, mapping its 0-100 onto
# [base, base+span] of the current part.
progress_track() {
    OVERALL_TRACK=1
    OVERALL_BASE="$1"
    OVERALL_SPAN="$2"
}

progress_end() {
    local message="${1:-}"
    PROGRESS_LAST=-1
    OVERALL_TRACK=0
    clear_line
    show_cursor
    [ -n "$message" ] && ok "$message"
    return 0
}

# For work with no measurable total (package managers, mostly). Shows elapsed
# time so a long apt run still looks alive.
spinner_start() {
    local label="$1"
    if [ "$IS_TTY" != 1 ]; then
        info "$label..."
        return 0
    fi
    hide_cursor
    label="$(fit_label "$label" 20)"
    (
        frames='|/-\'
        i=0
        start=$SECONDS
        while :; do
            printf '\r\033[2K    %s%s%s  %s %s(%ds)%s' \
                "$C_CYN" "${frames:$((i % 4)):1}" "$C_RESET" \
                "$label" "$C_DIM" "$((SECONDS - start))" "$C_RESET"
            i=$((i + 1))
            sleep 0.12
        done
    ) &
    SPINNER_PID=$!
}

spinner_stop() {
    local message="${1:-}"
    if [ -n "$SPINNER_PID" ]; then
        kill "$SPINNER_PID" 2>/dev/null || true
        wait "$SPINNER_PID" 2>/dev/null || true
        SPINNER_PID=""
    fi
    clear_line
    show_cursor
    [ -n "$message" ] && ok "$message"
    return 0
}

human_bytes() {
    local bytes="${1:-0}"
    if   [ "$bytes" -ge 1073741824 ]; then printf '%d.%dGB' $((bytes / 1073741824)) $(((bytes % 1073741824) * 10 / 1073741824))
    elif [ "$bytes" -ge 1048576 ];    then printf '%dMB' $((bytes / 1048576))
    elif [ "$bytes" -ge 1024 ];       then printf '%dKB' $((bytes / 1024))
    else printf '%dB' "$bytes"; fi
}

# Leaving the cursor hidden after a Ctrl-C would break the user's terminal.
cleanup() {
    [ -n "$SPINNER_PID" ] && { kill "$SPINNER_PID" 2>/dev/null || true; }
    show_cursor
}
trap cleanup EXIT INT TERM

STEP_NUM=0
STEP_TOTAL=5

# --------------------------------------------------------------------------
# Overall progress
#
# The five parts of an install are nothing like equal in length: building is
# minutes and checking CMake is milliseconds. A bar that moved a fifth per part
# would sit at 80% for almost the entire install, which is worse than no bar at
# all -- so each part carries a weight, and they are what the main bar counts.
#
# The numbers are rough measurements of a cold install on a mid-range machine,
# not guesses; they only have to be right about the shape.
# --------------------------------------------------------------------------
# Indexed by step number, so [0] is unused and the rest sum to 100.
STEP_WEIGHTS=(0 4 16 3 12 65)

# How far through the whole install we are, given `fraction` (0-100) of the
# part currently running.
overall_percent() {
    local fraction="${1:-0}" done=0 i
    for ((i = 1; i < STEP_NUM && i <= STEP_TOTAL; i++)); do
        done=$((done + STEP_WEIGHTS[i]))
    done
    if [ "$STEP_NUM" -ge 1 ] && [ "$STEP_NUM" -le "$STEP_TOTAL" ]; then
        done=$((done + STEP_WEIGHTS[STEP_NUM] * fraction / 100))
    fi
    [ "$done" -gt 100 ] && done=100
    printf '%s' "$done"
}

# The main bar. Drawn once per part rather than redrawn continuously, so it
# stays in the scrollback as a record of how far each part got -- and so it can
# never fight with the per-step bar for the same terminal row.
overall_bar() {
    local percent="$1" width=30 filled empty cols
    cols="$(term_cols)"
    [ "$cols" -lt 70 ] && width=18
    [ "$cols" -lt 50 ] && width=10
    [ "$percent" -lt 0 ]   && percent=0
    [ "$percent" -gt 100 ] && percent=100

    filled=$((percent * width / 100))
    empty=$((width - filled))

    # Green, where the per-step bar is cyan: at a glance the two are telling
    # you different things, and the colour is the fastest way to say so.
    printf '    %sinstall%s  %s%s%s%s%s %3d%%\n' \
        "$C_DIM" "$C_RESET" \
        "$C_GRN" "$(repeat_char "$BAR_FILL" "$filled")" \
        "$C_DIM" "$(repeat_char "$BAR_VOID" "$empty")" "$C_RESET" \
        "$percent"
}

step()  {
    STEP_NUM=$((STEP_NUM + 1))
    printf '\n%s==>%s %s[%d/%d]%s %s%s%s\n' \
        "$C_CYN" "$C_RESET" "$C_DIM" "$STEP_NUM" "$STEP_TOTAL" "$C_RESET" \
        "$C_BOLD" "$*" "$C_RESET"
    overall_bar "$(overall_percent 0)"
}
info()  { printf '    %s\n' "$*"; }
muted() { printf '    %s%s%s\n' "$C_DIM" "$*" "$C_RESET"; }
warn()  { printf '%s !! %s %s\n' "$C_YEL" "$C_RESET" "$*" >&2; }
ok()    { printf '    %s✓%s %s\n' "$C_GRN" "$C_RESET" "$*"; }
die()   { printf '\n%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

banner() {
cat <<'ART'

   /\           /\
  /  \_________/  \      BatBot
 |   ___________   |     a local roundtable of experts
 |  |           |  |
 |  |  o     o  |  |
 |  |    \_/    |  |
 |  |___________|  |
 |_________________|

ART
}

usage() {
    banner
    cat <<EOF
usage: install.sh [options]

  --gpu MODE       cuda | vulkan | cpu | auto     (default: auto)
                   auto detects your hardware and picks the best backend
                   it can actually build for.
  --prefix DIR     install location (default: /usr/local with sudo,
                   otherwise ~/.local)
  --branch NAME    git branch to build (default: main)
  --jobs N         parallel build jobs (default: all cores)
  --no-deps        do not install system packages
  -y, --yes        assume yes; never prompt
  --check          report what would be installed and which runtime is chosen
                   would be chosen, then exit without changing anything
  --uninstall      remove an installed batbot
  -h, --help       this message

examples:
  curl -fsSL $RAW_URL | bash
  curl -fsSL $RAW_URL | bash -s -- --gpu vulkan
  ./install.sh --prefix ~/.local --gpu cpu
EOF
}

# --------------------------------------------------------------------------
# Arguments
# --------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --gpu)       RUNTIME="${2:-}";    shift 2 ;;
        --gpu=*)     RUNTIME="${1#*=}";   shift ;;
        --prefix)    PREFIX="${2:-}"; shift 2 ;;
        --prefix=*)  PREFIX="${1#*=}";shift ;;
        --branch)    BRANCH="${2:-}"; shift 2 ;;
        --branch=*)  BRANCH="${1#*=}";shift ;;
        --jobs)      JOBS="${2:-}";   shift 2 ;;
        --jobs=*)    JOBS="${1#*=}";  shift ;;
        --no-deps)   INSTALL_DEPS=0;  shift ;;
        -y|--yes)    ASSUME_YES=1;    shift ;;
        --uninstall) DO_UNINSTALL=1;  shift ;;
        --check)     DO_CHECK=1;      shift ;;
        -h|--help)   usage; exit 0 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
done

case "$RUNTIME" in
    auto|cuda|vulkan|cpu) ;;
    *) die "--gpu must be one of: auto, cuda, vulkan, cpu" ;;
esac

# --------------------------------------------------------------------------
# Prompting
#
# The script is usually running as `curl | bash`, so stdin is the script itself
# and `read` would consume it. Questions go to the terminal directly, and when
# there is no terminal we take the safe default rather than hanging.
# --------------------------------------------------------------------------
confirm() {
    local prompt="$1" default="${2:-y}" reply
    if [ "$ASSUME_YES" = 1 ]; then return 0; fi
    if [ ! -r /dev/tty ]; then
        muted "no terminal to ask on; assuming '$default'"
        [ "$default" = "y" ]
        return
    fi
    local hint="[Y/n]"; [ "$default" = "n" ] && hint="[y/N]"
    printf '    %s %s ' "$prompt" "$hint" > /dev/tty
    read -r reply < /dev/tty || reply=""
    reply="${reply:-$default}"
    case "$reply" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# --------------------------------------------------------------------------
# Platform
# --------------------------------------------------------------------------
detect_platform() {
    [ "$(uname -s)" = "Linux" ] || die "this installer supports Linux only (found $(uname -s))."

    if [ "$(id -u)" -ne 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        else
            SUDO=""
            if [ "$INSTALL_DEPS" = 1 ]; then
                warn "not root and no sudo found; skipping package installation."
                INSTALL_DEPS=0
            fi
        fi
    fi

    local id="" like=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        id="${ID:-}"; like="${ID_LIKE:-}"
    fi

    case "$id $like" in
        *debian*|*ubuntu*|*mint*)               PKG="apt" ;;
        *fedora*|*rhel*|*centos*)               PKG="dnf" ;;
        *arch*|*manjaro*|*endeavouros*)         PKG="pacman" ;;
        *suse*)                                 PKG="zypper" ;;
        *)
            if   command -v apt-get >/dev/null 2>&1; then PKG="apt"
            elif command -v dnf     >/dev/null 2>&1; then PKG="dnf"
            elif command -v pacman  >/dev/null 2>&1; then PKG="pacman"
            elif command -v zypper  >/dev/null 2>&1; then PKG="zypper"
            else PKG="unknown"; fi ;;
    esac

    info "distribution : ${PRETTY_NAME:-unknown}"
    info "package tool : $PKG"

    # Ask for the sudo password now, while nothing is being drawn over. A
    # password prompt appearing underneath a running spinner is unreadable and
    # looks like a hang.
    if [ -n "$SUDO" ] && [ "$INSTALL_DEPS" = 1 ]; then
        if ! sudo -n true 2>/dev/null; then
            info "administrator access is needed to install packages"
            sudo -v || die "could not obtain sudo; re-run with --no-deps to skip package installation"
        fi
    fi

    if [ "$PKG" = "unknown" ] && [ "$INSTALL_DEPS" = 1 ]; then
        warn "unrecognised package manager; skipping dependency installation."
        warn "you will need: a C++20 compiler, cmake >= 3.24, git."
        INSTALL_DEPS=0
    fi
}

PKG_LOG=""

pkg_install() {
    [ $# -gt 0 ] || return 0
    local status=0
    PKG_LOG="$(mktemp)"

    # Package managers report progress in their own incompatible ways and none
    # of it is reliably parseable, so this is a spinner with an elapsed clock
    # rather than a bar that would have to lie about the percentage.
    spinner_start "installing $* "
    case "$PKG" in
        apt)    { $SUDO apt-get update -qq \
                  && DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq "$@"; } \
                  > "$PKG_LOG" 2>&1 || status=$? ;;
        dnf)    $SUDO dnf install -y -q "$@"                > "$PKG_LOG" 2>&1 || status=$? ;;
        pacman) $SUDO pacman -Sy --needed --noconfirm "$@"  > "$PKG_LOG" 2>&1 || status=$? ;;
        zypper) $SUDO zypper --non-interactive install -y "$@" > "$PKG_LOG" 2>&1 || status=$? ;;
        *)      spinner_stop; warn "cannot install $* automatically"; return 1 ;;
    esac

    if [ "$status" -eq 0 ]; then
        spinner_stop "installed $*"
    else
        spinner_stop
        warn "package installation failed:"
        tail -8 "$PKG_LOG" >&2 || true
    fi
    rm -f "$PKG_LOG"
    return "$status"
}

# Is a package available to install at all? Keeps us from failing the whole run
# on a package that this distro release simply does not carry.
pkg_available() {
    case "$PKG" in
        apt)    apt-cache policy "$1" 2>/dev/null | grep -q 'Candidate: [^(]' ;;
        dnf)    dnf list --available "$1" >/dev/null 2>&1 || dnf list --installed "$1" >/dev/null 2>&1 ;;
        pacman) pacman -Si "$1" >/dev/null 2>&1 ;;
        zypper) zypper --non-interactive info "$1" 2>/dev/null | grep -q '^Version' ;;
        *)      return 1 ;;
    esac
}

# --------------------------------------------------------------------------
# CMake
#
# BatBot needs CMake >= 3.24, which is newer than several current LTS releases
# ship. Rather than fail, fetch the official static build into a cache dir.
# --------------------------------------------------------------------------
cmake_version_ok() {
    local exe="$1" version major minor
    command -v "$exe" >/dev/null 2>&1 || return 1
    version="$("$exe" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    [ -n "$version" ] || return 1
    major="${version%%.*}"; minor="${version#*.}"; minor="${minor%%.*}"
    [ "$major" -gt "$CMAKE_MIN_MAJOR" ] && return 0
    [ "$major" -eq "$CMAKE_MIN_MAJOR" ] && [ "$minor" -ge "$CMAKE_MIN_MINOR" ]
}

# Download with a real progress bar.
#
# curl's own --progress-bar cannot be restyled and writes to stderr in a format
# that is not worth parsing, so the transfer runs in the background and the bar
# is driven by the size of the file on disk against Content-Length. Without a
# Content-Length (chunked responses) it degrades to a spinner rather than
# inventing a percentage.
download_with_progress() {
    local url="$1" out="$2" label="$3"
    local total="" pid status=0 now=0

    total="$(curl -fsSLI "$url" 2>/dev/null \
        | awk 'BEGIN{IGNORECASE=1} /^content-length:/ {v=$2} END{gsub(/[^0-9]/,"",v); print v}')"

    rm -f "$out"
    curl -fsSL "$url" -o "$out" &
    pid=$!

    if [ -n "$total" ] && [ "$total" -gt 0 ] 2>/dev/null && [ "$IS_TTY" = 1 ]; then
        progress_track 0 100
        progress_begin
        while kill -0 "$pid" 2>/dev/null; do
            now=0
            [ -f "$out" ] && now="$(stat -c %s "$out" 2>/dev/null || echo 0)"
            progress_render "$((now * 100 / total))" \
                "$label  $(human_bytes "$now") / $(human_bytes "$total")"
            sleep 0.15
        done
        wait "$pid" || status=$?
        [ "$status" -eq 0 ] && progress_render 100 "$label  $(human_bytes "$total")"
        progress_end "downloaded $label"
    else
        spinner_start "downloading $label "
        wait "$pid" || status=$?
        spinner_stop "downloaded $label"
    fi

    return "$status"
}

bootstrap_cmake() {
    local arch cache url tarball dir
    arch="$(uname -m)"
    case "$arch" in
        x86_64|amd64)  arch="x86_64" ;;
        aarch64|arm64) arch="aarch64" ;;
        *) die "no prebuilt CMake for $arch; please install cmake >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR} yourself." ;;
    esac

    cache="${XDG_CACHE_HOME:-$HOME/.cache}/batbot"
    dir="$cache/cmake-${CMAKE_BOOTSTRAP_VERSION}-linux-${arch}"
    mkdir -p "$cache"

    if [ ! -x "$dir/bin/cmake" ]; then
        url="https://github.com/Kitware/CMake/releases/download/v${CMAKE_BOOTSTRAP_VERSION}/cmake-${CMAKE_BOOTSTRAP_VERSION}-linux-${arch}.tar.gz"
        tarball="$cache/cmake.tar.gz"
        info "your CMake is older than ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}; fetching ${CMAKE_BOOTSTRAP_VERSION}"
        download_with_progress "$url" "$tarball" "CMake ${CMAKE_BOOTSTRAP_VERSION}" \
            || die "could not download CMake from $url"
        spinner_start "extracting CMake "
        tar -xzf "$tarball" -C "$cache"
        spinner_stop "extracted"
        rm -f "$tarball"
    fi

    [ -x "$dir/bin/cmake" ] || die "CMake bootstrap failed"
    CMAKE="$dir/bin/cmake"
    ok "using $($CMAKE --version | head -1) from cache"
}

ensure_cmake() {
    if cmake_version_ok cmake; then
        CMAKE="cmake"
        ok "$(cmake --version | head -1)"
    else
        bootstrap_cmake
    fi
}

# --------------------------------------------------------------------------
# GPU backend selection
# --------------------------------------------------------------------------

# Minimum CUDA toolkit that can generate code for a given compute capability.
# Getting this wrong is not a warning at build time -- it is a GPU that silently
# cannot be used, which is exactly what happens if you pair a Blackwell card
# with the CUDA 12.0 most distributions still ship.
cuda_required_for_cap() {
    local cap="$1" major minor
    major="${cap%%.*}"; minor="${cap#*.}"
    if   [ "$major" -ge 12 ]; then echo "12.8"
    elif [ "$major" -ge 10 ]; then echo "12.8"
    elif [ "$major" -eq 9  ]; then echo "12.0"
    elif [ "$major" -eq 8 ] && [ "$minor" -ge 9 ]; then echo "11.8"
    elif [ "$major" -eq 8 ]; then echo "11.1"
    else echo "11.0"; fi
}

version_ge() {
    # true when $1 >= $2, comparing major.minor numerically
    local a_major="${1%%.*}" a_minor="${1#*.}" b_major="${2%%.*}" b_minor="${2#*.}"
    a_minor="${a_minor%%.*}"; b_minor="${b_minor%%.*}"
    [ "$a_major" -gt "$b_major" ] && return 0
    [ "$a_major" -lt "$b_major" ] && return 1
    [ "${a_minor:-0}" -ge "${b_minor:-0}" ]
}

nvcc_version() {
    command -v nvcc >/dev/null 2>&1 || return 1
    nvcc --version 2>/dev/null | grep -oE 'release [0-9]+\.[0-9]+' | awk '{print $2}' | head -1
}

apt_cuda_candidate() {
    [ "$PKG" = "apt" ] || return 1
    apt-cache policy nvidia-cuda-toolkit 2>/dev/null \
        | awk '/Candidate:/ {print $2}' | grep -oE '^[0-9]+\.[0-9]+' | head -1
}

CUDA_NOTE=""

decide_backend() {
    local caps cap required="" highest="0.0" have_nvidia=0 detected

    if command -v nvidia-smi >/dev/null 2>&1; then
        caps="$(nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null || true)"
        if [ -n "$caps" ]; then
            have_nvidia=1
            while IFS= read -r line; do
                [ -n "$line" ] || continue
                info "GPU: $line"
                cap="$(printf '%s' "$line" | awk -F', ' '{print $2}' | tr -d ' ')"
                [ -n "$cap" ] || continue
                version_ge "$cap" "$highest" && highest="$cap"
            done <<< "$caps"
            required="$(cuda_required_for_cap "$highest")"
        fi
    fi

    if [ "$RUNTIME" != "auto" ]; then
        info "backend: $RUNTIME (requested)"
        return
    fi

    if [ "$have_nvidia" = 0 ]; then
        # No NVIDIA card. Vulkan still covers AMD and Intel GPUs.
        if [ "$PKG" != "unknown" ]; then
            RUNTIME="vulkan"
            info "no NVIDIA GPU detected; choosing Vulkan (covers AMD and Intel)"
        else
            RUNTIME="cpu"
        fi
        return
    fi

    # An NVIDIA card is present. Prefer CUDA, but only if a toolkit new enough
    # for the newest card is actually obtainable.
    local available=""
    available="$(nvcc_version || true)"
    [ -n "$available" ] || available="$(apt_cuda_candidate || true)"

    if [ -n "$available" ] && version_ge "$available" "$required"; then
        RUNTIME="cuda"
        info "CUDA $available covers compute capability $highest; choosing CUDA"
    else
        RUNTIME="vulkan"
        detected="${available:-none}"
        CUDA_NOTE="Your newest GPU is compute capability ${highest}, which needs CUDA >= ${required}.
    The CUDA toolkit available here is ${detected}, which cannot build for it, so
    Vulkan was chosen instead -- it works on all of your GPUs via the driver.
    For CUDA, install a newer toolkit from developer.nvidia.com/cuda-downloads
    and re-run with --gpu cuda."
        warn "CUDA ${detected} is too old for compute capability ${highest} (needs ${required}); using Vulkan"
    fi
}

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------
PKGS_BASE=()
PKGS_VULKAN=()
PKGS_CUDA=()

# Which packages this distribution needs for the chosen backend. Split out from
# the install so --check can report them without touching anything.
resolve_packages() {
    PKGS_BASE=(); PKGS_VULKAN=(); PKGS_CUDA=()
    case "$PKG" in
        apt)    PKGS_BASE=(build-essential cmake git pkg-config curl ca-certificates)
                # spirv-headers is the one that is easy to miss: ggml's Vulkan
                # backend does find_package(SPIRV-Headers CONFIG REQUIRED), and
                # without it the build dies at configure time with an error
                # naming a CMake package rather than anything installable.
                PKGS_VULKAN=(glslc libvulkan-dev spirv-headers)
                PKGS_CUDA=(nvidia-cuda-toolkit) ;;
        dnf)    PKGS_BASE=(gcc-c++ make cmake git pkgconf-pkg-config curl)
                PKGS_VULKAN=(glslc vulkan-loader-devel spirv-headers-devel)
                PKGS_CUDA=(cuda-toolkit) ;;
        pacman) PKGS_BASE=(base-devel cmake git curl)
                PKGS_VULKAN=(shaderc vulkan-headers vulkan-icd-loader spirv-headers)
                PKGS_CUDA=(cuda) ;;
        zypper) PKGS_BASE=(gcc-c++ make cmake git-core curl)
                PKGS_VULKAN=(shaderc vulkan-devel spirv-headers)
                PKGS_CUDA=(cuda) ;;
    esac
    return 0
}

# Are all of `$@` installable on this distribution?
packages_available() {
    local p
    for p in "$@"; do
        pkg_available "$p" || return 1
    done
    return 0
}

install_dependencies() {
    resolve_packages

    pkg_install "${PKGS_BASE[@]}" || die "could not install the build toolchain"

    # The Vulkan SDK goes in whatever backend was chosen, and it goes in
    # without asking. It is a few megabytes, it is what lets the settings
    # screen build a Vulkan runtime later without root, and needing sudo from
    # inside a TUI is the problem this avoids.
    if [ "${#PKGS_VULKAN[@]}" -gt 0 ]; then
        if packages_available "${PKGS_VULKAN[@]}"; then
            pkg_install "${PKGS_VULKAN[@]}" ||
                warn "the Vulkan SDK did not install; Vulkan runtimes cannot be built"
        else
            warn "no Vulkan SDK packages on this distribution; Vulkan runtimes cannot be built"
        fi
    fi

    # CUDA is different in kind: several gigabytes, and useless without an
    # NVIDIA card. It is only offered when the hardware asked for it.
    if [ "$RUNTIME" = "cuda" ]; then
        if ! packages_available "${PKGS_CUDA[@]}"; then
            warn "the CUDA toolkit is not packaged here; using Vulkan instead"
            RUNTIME="vulkan"
            return 0
        fi
        if command -v nvcc >/dev/null 2>&1; then
            return 0
        fi
        muted "the CUDA toolkit is a large download (often 2-4 GB)"
        if confirm "Install the CUDA toolkit now?" y; then
            pkg_install "${PKGS_CUDA[@]}" || {
                warn "the CUDA toolkit failed to install; using Vulkan instead"
                RUNTIME="vulkan"
            }
        else
            info "skipping CUDA -- you can install it later and add the runtime in settings"
            RUNTIME="vulkan"
        fi
    fi
    return 0
}

# --------------------------------------------------------------------------
# Source
# --------------------------------------------------------------------------
# git reports "Receiving objects:  45% (90/200)" on stderr under --progress,
# which is a genuine percentage worth showing on a slow connection.
git_clone_with_progress() {
    local url="$1" dest="$2" branch="$3" status=0 log
    log="$(mktemp)"

    progress_track 0 100
    progress_begin
    set +e
    git clone --depth 1 --branch "$branch" --progress "$url" "$dest" 2>&1 \
        | while IFS= read -r line; do
              printf '%s\n' "$line" >> "$log"
              case "$line" in
                  *"Receiving objects:"*|*"Resolving deltas:"*)
                      percent="${line#*: }"
                      percent="${percent%%\%*}"
                      percent="${percent// /}"
                      case "$percent" in
                          ''|*[!0-9]*) ;;
                          *) progress_render "$percent" "${line%%:*}" ;;
                      esac
                      ;;
              esac
          done
    status="${PIPESTATUS[0]}"
    set -e
    progress_end

    if [ "$status" -ne 0 ]; then
        show_log_tail "$log" 15
        rm -f "$log"
        die "could not clone $url"
    fi
    rm -f "$log"
    ok "cloned $branch"
}

locate_source() {
    local here=""
    if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]:-}" ]; then
        here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    fi

    # Running ./install.sh from inside a checkout builds that checkout, so you
    # can test local changes without pushing them first.
    if [ -n "$here" ] && [ -f "$here/CMakeLists.txt" ] && grep -q 'project(batbot' "$here/CMakeLists.txt" 2>/dev/null; then
        SRC_DIR="$here"
        info "building the checkout at $SRC_DIR"
        return
    fi

    SRC_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/batbot/src"
    if [ -d "$SRC_DIR/.git" ]; then
        spinner_start "updating $SRC_DIR "
        git -C "$SRC_DIR" fetch --depth 1 origin "$BRANCH" --quiet >/dev/null 2>&1
        git -C "$SRC_DIR" checkout --quiet FETCH_HEAD >/dev/null 2>&1
        spinner_stop "updated to latest $BRANCH"
    else
        info "cloning into $SRC_DIR"
        mkdir -p "$(dirname "$SRC_DIR")"
        rm -rf "$SRC_DIR"
        git_clone_with_progress "$REPO_URL" "$SRC_DIR" "$BRANCH"
        CLONED_FRESH=1
    fi
}

# --------------------------------------------------------------------------
# Build & install
# --------------------------------------------------------------------------
choose_prefix() {
    if [ -n "$PREFIX" ]; then return; fi
    if [ "$(id -u)" -eq 0 ] || [ -n "$SUDO" ]; then
        PREFIX="/usr/local"
    else
        PREFIX="$HOME/.local"
    fi
}

BUILD_LOG=""

# Show why a step failed without burying the user in thousands of lines.
show_log_tail() {
    local log="$1" lines="${2:-25}"
    [ -f "$log" ] || return 0
    printf '\n%s--- last %d lines of %s ---%s\n' "$C_DIM" "$lines" "$log" "$C_RESET" >&2
    tail -n "$lines" "$log" >&2
    printf '%s--- end ---%s\n\n' "$C_DIM" "$C_RESET" >&2
}

# Where to build.
#
# Loadable runtimes are shared libraries, and shared libraries need symlinks to
# carry their version suffix. A checkout on exFAT or NTFS -- an external drive
# shared with Windows, say -- cannot make one, and the link step fails with
# "Operation not permitted" a long way into the build. So the build tree is
# only kept beside the source when the filesystem can support it, and moved to
# the cache directory when it cannot.
choose_build_dir() {
    local candidate="$SRC_DIR/build"
    mkdir -p "$candidate" 2>/dev/null || true

    if ln -sfn "$candidate" "$candidate/.symlink-probe" 2>/dev/null; then
        rm -f "$candidate/.symlink-probe"
        BUILD_DIR="$candidate"
        return 0
    fi

    # Do not leave the empty probe directory behind in the checkout.
    rmdir "$candidate" 2>/dev/null || true

    BUILD_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/batbot/build"
    mkdir -p "$BUILD_DIR"
    info "this filesystem has no symlinks; building in $BUILD_DIR instead"
    return 0
}

build_and_install() {
    local status=0
    choose_build_dir
    local build_dir="$BUILD_DIR"

    # The log is only interesting when something breaks, so its path is
    # announced on failure rather than upfront, and it is removed on success
    # instead of accumulating in /tmp on every upgrade.
    BUILD_LOG="$(mktemp -t batbot-build-XXXXXX.log)"

    # --- configure ---------------------------------------------------------
    # The first configure clones llama.cpp and FTXUI, so it is slow and has no
    # percentage of its own.
    # No GPU backend is compiled in. BatBot is built with ggml's loadable
    # backends, so CUDA and Vulkan are files the settings screen manages rather
    # than a decision frozen here -- which is the whole point of this install
    # producing something you can change your mind about later.
    spinner_start "configuring (loadable runtimes) "
    "$CMAKE" -S "$SRC_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBATBOT_BACKEND_DL=ON \
        > "$BUILD_LOG" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        spinner_stop
        show_log_tail "$BUILD_LOG"
        die "cmake configure failed. The full log is at $BUILD_LOG"
    fi
    spinner_stop "configured"

    # --- compile -----------------------------------------------------------
    # CMake's Makefile generator prints "[ 42%] Building ..." for every unit,
    # which is a real, ordered percentage worth turning into a bar. The full
    # output still goes to the log so a failure can be diagnosed.
    info "compiling with $JOBS jobs (several minutes on a first build)"
    # Part 5 also builds a GPU runtime afterwards, so this compile is only the
    # first stretch of it -- otherwise the overall figure would reach 100%
    # with minutes of work still to go.
    progress_track 5 65
    progress_begin
    status=0
    set +e
    "$CMAKE" --build "$build_dir" -j "$JOBS" 2>&1 \
        | while IFS= read -r line; do
              printf '%s\n' "$line" >> "$BUILD_LOG"
              case "$line" in
                  \[*%\]*)
                      percent="${line#*[}"
                      percent="${percent%%\%*}"
                      percent="${percent// /}"
                      case "$percent" in
                          ''|*[!0-9]*) ;;
                          *)
                              target="${line#*] }"
                              progress_render "$percent" "${target:0:30}"
                              ;;
                      esac
                      ;;
              esac
          done
    status="${PIPESTATUS[0]}"
    set -e

    if [ "$status" -ne 0 ]; then
        progress_end
        show_log_tail "$BUILD_LOG" 30
        die "build failed. The full log is at $BUILD_LOG"
    fi
    progress_render 100 "done"
    progress_end "compiled"

    # --- tests -------------------------------------------------------------
    spinner_start "running tests "
    if (cd "$build_dir" && ctest --output-on-failure >> "$BUILD_LOG" 2>&1); then
        spinner_stop "tests passed"
    else
        spinner_stop
        warn "tests did not pass; installing anyway (please report this)"
        warn "details are in $BUILD_LOG"
        KEEP_BUILD_LOG=1
    fi

    # --- install -----------------------------------------------------------
    spinner_start "installing to ${PREFIX}/bin "
    status=0
    if [ -w "$PREFIX" ] || [ "$(id -u)" -eq 0 ]; then
        "$CMAKE" --install "$build_dir" --component batbot >> "$BUILD_LOG" 2>&1 || status=$?
    else
        mkdir -p "$PREFIX/bin" 2>/dev/null || true
        if [ -w "$PREFIX/bin" ]; then
            "$CMAKE" --install "$build_dir" --component batbot >> "$BUILD_LOG" 2>&1 || status=$?
        else
            $SUDO "$CMAKE" --install "$build_dir" --component batbot >> "$BUILD_LOG" 2>&1 || status=$?
        fi
    fi
    if [ "$status" -ne 0 ]; then
        spinner_stop
        show_log_tail "$BUILD_LOG"
        die "install failed"
    fi
    spinner_stop "installed"

    seed_runtime_source "$build_dir"
    prebuild_runtime

    if [ -z "${KEEP_BUILD_LOG:-}" ]; then
        rm -f "$BUILD_LOG"
        BUILD_LOG=""
    fi
}

# --------------------------------------------------------------------------
# Runtimes
#
# BatBot ships with the CPU runtime and builds GPU ones on demand, from the
# settings screen. Both of those need a llama.cpp checkout at the exact tag the
# binary was built against -- and the build we just did already downloaded one.
# Copying it here means adding a runtime later needs no network at all.
# --------------------------------------------------------------------------
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/batbot"

seed_runtime_source() {
    local build_dir="$1"
    local fetched="$build_dir/_deps/llama-src"
    local target="$DATA_DIR/runtime-src"

    [ -d "$fetched" ] || return 0
    if [ -f "$target/CMakeLists.txt" ]; then
        return 0
    fi

    spinner_start "saving the llama.cpp source for future runtimes "
    mkdir -p "$DATA_DIR"
    rm -rf "$target"
    # Without .git this is a fraction of the size and still builds.
    if cp -r "$fetched" "$target" 2>/dev/null; then
        rm -rf "$target/.git"
        spinner_stop "runtime source ready ($(du -sh "$target" 2>/dev/null | cut -f1))"
    else
        spinner_stop
        warn "could not copy the llama.cpp source; adding a runtime later will re-download it"
    fi
    return 0
}

# Build one GPU runtime now, so the first run is already accelerated. Exactly
# what the settings screen would do, and skippable -- the point of the whole
# design is that this is never a one-time decision.
prebuild_runtime() {
    local kind="$RUNTIME" option tool target_dir src status=0

    # Check for the compiler the backend needs rather than for whether we
    # installed it: --no-deps users often already have the SDK, and this is
    # the same test the in-app runtime builder makes.
    case "$kind" in
        cuda)   option=GGML_CUDA;   tool=nvcc  ;;
        vulkan) option=GGML_VULKAN; tool=glslc ;;
        # cpu ships with the binary, and "none" is an explicit opt-out.
        *)      return 0 ;;
    esac

    if ! command -v "$tool" >/dev/null 2>&1; then
        warn "$tool is not installed, so the $kind runtime was not built"
        muted "install it, then add the runtime from settings (ctrl-e, Runtimes)"
        return 0
    fi

    src="$DATA_DIR/runtime-src"
    [ -f "$src/CMakeLists.txt" ] || return 0

    local build_dir="$DATA_DIR/runtime-build/$kind"
    target_dir="$DATA_DIR/runtimes"
    mkdir -p "$build_dir" "$target_dir"

    RUNTIME_LOG="$(mktemp -t batbot-runtime-XXXXXX.log)"

    spinner_start "configuring the $kind runtime "
    "$CMAKE" -S "$src" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DGGML_BACKEND_DL=ON \
        -DGGML_NATIVE=OFF \
        -DGGML_CPU=OFF \
        -D${option}=ON \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
        -DLLAMA_BUILD_COMMON=OFF -DLLAMA_CURL=OFF \
        > "$RUNTIME_LOG" 2>&1 || status=$?

    if [ "$status" -ne 0 ]; then
        spinner_stop
        show_log_tail "$RUNTIME_LOG" 20
        warn "the $kind runtime could not be configured; BatBot will run on CPU"
        warn "you can retry from the settings screen (ctrl-e, Runtimes)"
        return 0
    fi
    spinner_stop "configured the $kind runtime"

    info "compiling the $kind runtime (this is the long part)"
    progress_track 75 25
    progress_begin
    status=0
    set +e
    "$CMAKE" --build "$build_dir" --target ggml -j "$JOBS" 2>&1 \
        | while IFS= read -r line; do
              printf '%s\n' "$line" >> "$RUNTIME_LOG"
              case "$line" in
                  \[*%\]*)
                      percent="${line#*[}"
                      percent="${percent%%\%*}"
                      percent="${percent// /}"
                      case "$percent" in
                          ''|*[!0-9]*) ;;
                          *) target="${line#*] }"; progress_render "$percent" "${target:0:30}" ;;
                      esac
                      ;;
              esac
          done
    status="${PIPESTATUS[0]}"
    set -e
    progress_end

    if [ "$status" -ne 0 ]; then
        show_log_tail "$RUNTIME_LOG" 20
        warn "the $kind runtime failed to build; BatBot will run on CPU"
        warn "the full log is at $RUNTIME_LOG"
        warn "you can retry from the settings screen (ctrl-e, Runtimes)"
        return 0
    fi

    spinner_start "installing the $kind runtime "
    local copied=0 f
    for f in "$build_dir"/bin/libggml-"$kind"*.so; do
        [ -f "$f" ] || continue
        cp "$f" "$target_dir/" && copied=$((copied + 1))
    done
    if [ "$copied" -eq 0 ]; then
        spinner_stop
        warn "the $kind build produced no module; BatBot will run on CPU"
        return 0
    fi

    # The manifest is what the settings screen reads to say where a runtime
    # came from, so write it the same way the in-app builder does.
    printf '{\n  "%s": {\n    "llama_tag": "%s",\n    "built_at": "%s"\n  }\n}\n' \
        "$kind" "$LLAMA_TAG" "$(date -u '+%Y-%m-%d %H:%M UTC')" \
        > "$target_dir/manifest.json"

    RUNTIME_INSTALLED="$kind"
    spinner_stop "$kind runtime installed"
    rm -f "$RUNTIME_LOG"
    return 0
}

uninstall() {
    banner
    local found=0 p
    for p in "$PREFIX" /usr/local "$HOME/.local"; do
        [ -n "$p" ] || continue
        if [ -f "$p/bin/batbot" ]; then
            # Not step(): uninstall is a single action, not one of the five
            # phases of an install, so a "[1/5]" counter would be nonsense.
            printf '\n%s==>%s %sRemoving %s/bin/batbot%s\n' \
                "$C_CYN" "$C_RESET" "$C_BOLD" "$p" "$C_RESET"
            if [ -w "$p/bin" ]; then rm -f "$p/bin/batbot"; else $SUDO rm -f "$p/bin/batbot"; fi
            found=1
        fi
    done
    [ "$found" = 1 ] || info "no installed batbot found"

    local src="${XDG_DATA_HOME:-$HOME/.local/share}/batbot/src"
    if [ -d "$src" ] && confirm "Also remove the build checkout at $src?" n; then
        rm -rf "$src"
    fi
    info "your config and models were left alone:"
    muted "${XDG_CONFIG_HOME:-$HOME/.config}/batbot"
    exit 0
}

# --------------------------------------------------------------------------
# PATH advice
# --------------------------------------------------------------------------
path_advice() {
    case ":$PATH:" in
        *":$PREFIX/bin:"*) return ;;
    esac

    printf '\n'
    warn "$PREFIX/bin is not on your PATH."
    # Parameter expansion rather than basename: no external command, so this
    # still works if PATH is minimal or unusual.
    case "${SHELL##*/}" in
        fish)
            info "add it with:"
            printf '        %sfish_add_path %s/bin%s\n' "$C_BOLD" "$PREFIX" "$C_RESET" ;;
        zsh)
            info "add it with:"
            printf '        %secho '\''export PATH="%s/bin:$PATH"'\'' >> ~/.zshrc%s\n' "$C_BOLD" "$PREFIX" "$C_RESET" ;;
        *)
            info "add it with:"
            printf '        %secho '\''export PATH="%s/bin:$PATH"'\'' >> ~/.bashrc%s\n' "$C_BOLD" "$PREFIX" "$C_RESET" ;;
    esac
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
# Report what an install would do, changing nothing. Deliberately never asks
# for sudo, so it is safe to run anywhere -- including on someone else's box
# before deciding whether to install at all.
run_check() {
    local saved_deps="$INSTALL_DEPS"
    INSTALL_DEPS=0          # suppresses the sudo pre-authorisation
    banner
    printf '%s==>%s %sDry run -- nothing will be changed%s\n\n' \
        "$C_CYN" "$C_RESET" "$C_BOLD" "$C_RESET"

    detect_platform
    printf '\n'
    decide_backend
    INSTALL_DEPS="$saved_deps"

    printf '\n'
    if cmake_version_ok cmake; then
        ok "$(cmake --version | head -1)"
    else
        info "cmake is missing or older than ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR};"
        info "CMake ${CMAKE_BOOTSTRAP_VERSION} would be downloaded to ${XDG_CACHE_HOME:-$HOME/.cache}/batbot"
    fi

    printf '\n%swould install:%s\n' "$C_BOLD" "$C_RESET"
    if [ "$INSTALL_DEPS" = 1 ]; then
        resolve_packages
        info "toolchain  : ${PKGS_BASE[*]:-none}"
        info "vulkan sdk : ${PKGS_VULKAN[*]:-none available}"
        if [ "$RUNTIME" = "cuda" ]; then
            info "cuda sdk   : ${PKGS_CUDA[*]:-none available}  (you will be asked first)"
        else
            muted "cuda sdk   : not needed for the $RUNTIME runtime"
        fi
    else
        muted "(package installation disabled with --no-deps)"
    fi

    printf '\n%swould build and install:%s\n' "$C_BOLD" "$C_RESET"
    info "binary     : $PREFIX/bin/batbot"
    info "libraries  : $PREFIX/lib/batbot"
    info "config     : ${XDG_CONFIG_HOME:-$HOME/.config}/batbot/config.json"
    info "runtimes   : ${XDG_DATA_HOME:-$HOME/.local/share}/batbot/runtimes"
    if [ "$RUNTIME" = "cpu" ] || [ "$RUNTIME" = "none" ]; then
        info "prebuild   : CPU only -- add a GPU runtime later in settings"
    else
        info "prebuild   : the $RUNTIME runtime, on top of CPU"
    fi
    muted "GPU backends are loadable: they can be added or removed at any time"
    muted "from the settings screen, without rebuilding BatBot."

    if [ -n "$CUDA_NOTE" ]; then
        printf '\n'
        warn "About the GPU backend:"
        printf '    %s\n' "$CUDA_NOTE"
    fi
    printf '\n'
    exit 0
}

main() {
    choose_prefix
    [ "$DO_CHECK" = 1 ] && run_check
    banner
    [ "$DO_UNINSTALL" = 1 ] && uninstall

    step "Checking your system"
    detect_platform
    decide_backend

    if [ "$INSTALL_DEPS" = 1 ]; then
        step "Installing dependencies"
        install_dependencies
    else
        step "Skipping dependency installation (--no-deps)"
    fi

    step "Checking CMake"
    ensure_cmake

    step "Getting the source"
    command -v git >/dev/null 2>&1 || die "git is required but not installed"
    locate_source

    step "Building BatBot"
    build_and_install

    # Create the models directory now, so the first run has somewhere obvious to
    # put GGUFs rather than reporting a path that does not exist yet.
    local models_dir="${XDG_DATA_HOME:-$HOME/.local/share}/batbot/models"
    mkdir -p "$models_dir" 2>/dev/null || true

    # Close the main bar out at 100%, so the last thing on screen agrees with
    # the five that came before it rather than leaving it stuck at 96%.
    STEP_NUM=$((STEP_TOTAL + 1))
    printf '\n'
    overall_bar 100

    printf '\n%s%s  BatBot is installed.%s\n\n' "$C_GRN" "$C_BOLD" "$C_RESET"
    info "binary   : $PREFIX/bin/batbot"
    info "models   : $models_dir"
    info "config   : ${XDG_CONFIG_HOME:-$HOME/.config}/batbot/config.json"
    if [ -n "${RUNTIME_INSTALLED:-}" ]; then
        info "runtimes : CPU + ${RUNTIME_INSTALLED}"
    else
        info "runtimes : CPU"
        muted "add a GPU runtime any time: run batbot, ctrl-e, open Runtimes"
    fi

    if [ -n "$CUDA_NOTE" ]; then
        printf '\n'
        warn "About the GPU backend:"
        printf '    %s\n' "$CUDA_NOTE"
    fi

    path_advice

    cat <<EOF

  ${C_BOLD}Next:${C_RESET} BatBot ships no models. Bring your own GGUFs.

    1. put your .gguf files in ${C_BOLD}$models_dir${C_RESET}
    2. run ${C_BOLD}batbot${C_RESET} and press ${C_BOLD}ctrl-e${C_RESET} to assign a model to each
       expert seat and to the delegator, then ${C_BOLD}ctrl-s${C_RESET} to save
    3. cd into any project and run ${C_BOLD}batbot${C_RESET}

  A good delegator is any small instruct model, around 1B parameters.
  Check how well one routes before committing to it:

    ${C_BOLD}batbot-routebench <your-model.gguf>${C_RESET}

  To remove BatBot later:  ${C_BOLD}batbot --uninstall${C_RESET}

EOF
}

# Sourcing with BATBOT_INSTALL_LIB=1 loads the helpers without running anything,
# which is how tests/test_install.sh checks the version and backend logic.
if [ -z "${BATBOT_INSTALL_LIB:-}" ]; then
    main "$@"
fi
