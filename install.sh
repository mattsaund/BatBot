#!/usr/bin/env bash
#
# BatBot installer.
#
#   curl -fsSL https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh | bash
#
# Installs the build toolchain, picks and installs a GPU backend, builds BatBot,
# and puts the binary on your PATH. Safe to re-run: it upgrades in place.

set -euo pipefail

REPO_URL="https://github.com/mattsaund/batbot.git"
RAW_URL="https://raw.githubusercontent.com/mattsaund/batbot/main/install.sh"

BRANCH="main"
GPU="auto"
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

progress_end() {
    local message="${1:-}"
    PROGRESS_LAST=-1
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

step()  {
    STEP_NUM=$((STEP_NUM + 1))
    printf '\n%s==>%s %s[%d/%d]%s %s%s%s\n' \
        "$C_CYN" "$C_RESET" "$C_DIM" "$STEP_NUM" "$STEP_TOTAL" "$C_RESET" \
        "$C_BOLD" "$*" "$C_RESET"
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
  --check          report what would be installed and which GPU backend
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
        --gpu)       GPU="${2:-}";    shift 2 ;;
        --gpu=*)     GPU="${1#*=}";   shift ;;
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

case "$GPU" in
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

    if [ "$GPU" != "auto" ]; then
        info "backend: $GPU (requested)"
        return
    fi

    if [ "$have_nvidia" = 0 ]; then
        # No NVIDIA card. Vulkan still covers AMD and Intel GPUs.
        if [ "$PKG" != "unknown" ]; then
            GPU="vulkan"
            info "no NVIDIA GPU detected; choosing Vulkan (covers AMD and Intel)"
        else
            GPU="cpu"
        fi
        return
    fi

    # An NVIDIA card is present. Prefer CUDA, but only if a toolkit new enough
    # for the newest card is actually obtainable.
    local available=""
    available="$(nvcc_version || true)"
    [ -n "$available" ] || available="$(apt_cuda_candidate || true)"

    if [ -n "$available" ] && version_ge "$available" "$required"; then
        GPU="cuda"
        info "CUDA $available covers compute capability $highest; choosing CUDA"
    else
        GPU="vulkan"
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
PKGS_GPU=()

# Which packages this distribution needs for the chosen backend. Split out from
# the install so --check can report them without touching anything.
resolve_packages() {
    PKGS_BASE=(); PKGS_GPU=()
    case "$PKG" in
        apt)    PKGS_BASE=(build-essential cmake git pkg-config curl ca-certificates)
                [ "$GPU" = "vulkan" ] && PKGS_GPU=(glslc libvulkan-dev)
                [ "$GPU" = "cuda" ]   && PKGS_GPU=(nvidia-cuda-toolkit) ;;
        dnf)    PKGS_BASE=(gcc-c++ make cmake git pkgconf-pkg-config curl)
                [ "$GPU" = "vulkan" ] && PKGS_GPU=(glslc vulkan-loader-devel)
                [ "$GPU" = "cuda" ]   && PKGS_GPU=(cuda-toolkit) ;;
        pacman) PKGS_BASE=(base-devel cmake git curl)
                [ "$GPU" = "vulkan" ] && PKGS_GPU=(shaderc vulkan-headers vulkan-icd-loader)
                [ "$GPU" = "cuda" ]   && PKGS_GPU=(cuda) ;;
        zypper) PKGS_BASE=(gcc-c++ make cmake git-core curl)
                [ "$GPU" = "vulkan" ] && PKGS_GPU=(shaderc vulkan-devel)
                [ "$GPU" = "cuda" ]   && PKGS_GPU=(cuda) ;;
    esac
    # The trailing `[ ... ] && ...` above returns 1 whenever the backend is not
    # CUDA, and under `set -e` that would abort the caller. Return success
    # explicitly.
    return 0
}

install_dependencies() {
    local base=() gpu_pkgs=()
    resolve_packages
    base=("${PKGS_BASE[@]}")
    gpu_pkgs=("${PKGS_GPU[@]}")

    pkg_install "${base[@]}" || die "could not install the build toolchain"

    if [ "${#gpu_pkgs[@]}" -gt 0 ]; then
        local missing=()
        for p in "${gpu_pkgs[@]}"; do
            pkg_available "$p" || missing+=("$p")
        done

        if [ "${#missing[@]}" -gt 0 ]; then
            warn "not available on this distribution: ${missing[*]}"
            warn "falling back to a CPU-only build."
            GPU="cpu"
            return
        fi

        if [ "$GPU" = "cuda" ]; then
            muted "the CUDA toolkit is a large download (often 2-4 GB)"
            if ! confirm "Install the CUDA toolkit now?" y; then
                info "skipping CUDA; building for Vulkan instead"
                GPU="vulkan"
                install_dependencies
                return
            fi
        fi

        if ! pkg_install "${gpu_pkgs[@]}"; then
            warn "GPU packages failed to install; falling back to a CPU-only build."
            GPU="cpu"
        fi
    fi
}

# --------------------------------------------------------------------------
# Source
# --------------------------------------------------------------------------
# git reports "Receiving objects:  45% (90/200)" on stderr under --progress,
# which is a genuine percentage worth showing on a slow connection.
git_clone_with_progress() {
    local url="$1" dest="$2" branch="$3" status=0 log
    log="$(mktemp)"

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

build_and_install() {
    local build_dir="$SRC_DIR/build" cuda_flag=OFF vulkan_flag=OFF status=0
    [ "$GPU" = "cuda" ]   && cuda_flag=ON
    [ "$GPU" = "vulkan" ] && vulkan_flag=ON

    # The log is only interesting when something breaks, so its path is
    # announced on failure rather than upfront, and it is removed on success
    # instead of accumulating in /tmp on every upgrade.
    BUILD_LOG="$(mktemp -t batbot-build-XXXXXX.log)"

    # --- configure ---------------------------------------------------------
    # The first configure clones llama.cpp and FTXUI, so it is slow and has no
    # percentage of its own.
    spinner_start "configuring (backend: $GPU) "
    "$CMAKE" -S "$SRC_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBATBOT_CUDA="$cuda_flag" \
        -DBATBOT_VULKAN="$vulkan_flag" \
        > "$BUILD_LOG" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        spinner_stop
        show_log_tail "$BUILD_LOG"
        die "cmake configure failed. Re-run with --gpu cpu to rule out the GPU backend."
    fi
    spinner_stop "configured for $GPU"

    # --- compile -----------------------------------------------------------
    # CMake's Makefile generator prints "[ 42%] Building ..." for every unit,
    # which is a real, ordered percentage worth turning into a bar. The full
    # output still goes to the log so a failure can be diagnosed.
    info "compiling with $JOBS jobs (several minutes on a first build)"
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
        "$CMAKE" --install "$build_dir" >> "$BUILD_LOG" 2>&1 || status=$?
    else
        mkdir -p "$PREFIX/bin" 2>/dev/null || true
        if [ -w "$PREFIX/bin" ]; then
            "$CMAKE" --install "$build_dir" >> "$BUILD_LOG" 2>&1 || status=$?
        else
            $SUDO "$CMAKE" --install "$build_dir" >> "$BUILD_LOG" 2>&1 || status=$?
        fi
    fi
    if [ "$status" -ne 0 ]; then
        spinner_stop
        show_log_tail "$BUILD_LOG"
        die "install failed"
    fi
    spinner_stop "installed"

    if [ -z "${KEEP_BUILD_LOG:-}" ]; then
        rm -f "$BUILD_LOG"
        BUILD_LOG=""
    fi
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
        info "toolchain : ${PKGS_BASE[*]:-none}"
        info "backend   : ${PKGS_GPU[*]:-none needed}"
    else
        muted "(package installation disabled with --no-deps)"
    fi

    printf '\n%swould build and install:%s\n' "$C_BOLD" "$C_RESET"
    info "backend   : $GPU"
    info "binary    : $PREFIX/bin/batbot"
    info "config    : ${XDG_CONFIG_HOME:-$HOME/.config}/batbot/config.json"

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

    printf '\n%s%s  BatBot is installed.%s\n\n' "$C_GRN" "$C_BOLD" "$C_RESET"
    info "binary   : $PREFIX/bin/batbot"
    info "backend  : $GPU"
    info "models   : $models_dir"
    info "config   : ${XDG_CONFIG_HOME:-$HOME/.config}/batbot/config.json"

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
