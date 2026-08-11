#!/usr/bin/env bash
#
# Portable SuperSlicer Linux build bootstrap.
#
# The script probes host capabilities first, maps missing capabilities to the
# active package manager only when installation is requested, then builds the
# project against its bundled dependency set. Bundled dependencies keep the
# application build independent of whichever library versions a distro ships.

set -Eeuo pipefail
IFS=$'\n\t'

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$ROOT_DIR"

BUILD_TYPE="Release"
GTK_VERSION="3"
BUILD_DEPS=0
BUILD_SLIC3R=0
BUILD_TESTS=0
BUILD_PACKAGE=0
UPDATE_POT=0
INSTALL_MISSING=0
CHECK_ONLY=0
CONFIGURE_ONLY=0
WIPE=0
CLEAN_DEP_STAMPS=0
VERSION_DATE=0
JOBS="${JOBS:-}"

usage() {
    cat <<'USAGE'
Usage: ./BuildLinux.sh [options]

With no options, checks the host, builds bundled dependencies, and builds
SuperSlicer. Existing short options remain compatible.

Build selection:
  -d, --deps              build bundled dependencies
  -s, --slicer            build SuperSlicer
  -t, --tests             include tests when building SuperSlicer
  -i, --package           also create the .tgz/AppImage package
  -l, --update-pot        update the localization template
  -b, --debug             build Debug instead of Release
  -g, --gtk2              use GTK2 instead of GTK3
  -v, --version-date      replace +UNKNOWN with today's date
      --configure-only    configure selected builds without compiling
  -j, --jobs N            cap parallel build jobs

Host setup:
      --check             only report detected and missing prerequisites
  -u, --install           install missing prerequisites, then stop unless a
                          build option is also supplied

Cleanup:
  -w, --wipe              remove the selected generated build directories
  -r, --clean-deps        remove dependency build stamps before rebuilding

  -h, --help              show this help

Examples:
  ./BuildLinux.sh
  ./BuildLinux.sh --check
  ./BuildLinux.sh --install
  ./BuildLinux.sh --install --deps --slicer
  ./BuildLinux.sh --jobs 4
USAGE
}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

log() {
    printf '\n==> %s\n' "$*"
}

have_command() {
    command -v "$1" >/dev/null 2>&1
}

# Join arguments with an explicit separator. A bare "$*" cannot be used for this:
# the script sets IFS=$'\n\t', so "$*" splices newlines into the middle of a message.
join_by() {
    local separator="$1"
    shift
    local joined=""
    local item
    for item in "$@"; do
        joined+="${joined:+$separator}$item"
    done
    printf '%s' "$joined"
}

option_selected=0
while (($#)); do
    case "$1" in
        -d|--deps)
            BUILD_DEPS=1
            option_selected=1
            ;;
        -s|--slicer)
            BUILD_SLIC3R=1
            option_selected=1
            ;;
        -t|--tests)
            BUILD_TESTS=1
            option_selected=1
            ;;
        -i|--package)
            BUILD_PACKAGE=1
            BUILD_SLIC3R=1
            option_selected=1
            ;;
        -l|--update-pot)
            UPDATE_POT=1
            BUILD_SLIC3R=1
            option_selected=1
            ;;
        -b|--debug)
            BUILD_TYPE="Debug"
            option_selected=1
            ;;
        -g|--gtk2)
            GTK_VERSION="2"
            option_selected=1
            ;;
        -v|--version-date)
            VERSION_DATE=1
            option_selected=1
            ;;
        --configure-only)
            CONFIGURE_ONLY=1
            option_selected=1
            ;;
        --check)
            CHECK_ONLY=1
            ;;
        -u|--install)
            INSTALL_MISSING=1
            ;;
        -w|--wipe)
            WIPE=1
            option_selected=1
            ;;
        -r|--clean-deps)
            CLEAN_DEP_STAMPS=1
            BUILD_DEPS=1
            option_selected=1
            ;;
        -j|--jobs)
            (($# >= 2)) || die "$1 requires a job count"
            JOBS="$2"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1 (run --help)"
            ;;
    esac
    shift
done

if ((option_selected == 0 && CHECK_ONLY == 0 && INSTALL_MISSING == 0)); then
    BUILD_DEPS=1
    BUILD_SLIC3R=1
fi

[[ "$(uname -s)" == "Linux" ]] || die "this script supports Linux hosts only"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64)
        ARCH="x86_64"
        ;;
    aarch64|arm64)
        ARCH="aarch64"
        ;;
    armv7l|armv7)
        ARCH="armv7"
        ;;
    *)
        die "unsupported CPU architecture: $ARCH"
        ;;
esac

OS_ID="unknown"
OS_NAME="Linux"
OS_LIKE=""
if [[ -r /etc/os-release ]]; then
    # os-release is defined as shell-compatible variable assignments.
    # shellcheck disable=SC1091
    source /etc/os-release
    OS_ID="${ID:-unknown}"
    OS_NAME="${PRETTY_NAME:-${NAME:-Linux}}"
    OS_LIKE="${ID_LIKE:-}"
fi

PACKAGE_MANAGER=""
for candidate in apt-get dnf yum pacman zypper apk; do
    if have_command "$candidate"; then
        PACKAGE_MANAGER="$candidate"
        break
    fi
done

MISSING=()

need_command() {
    local label="$1"
    shift
    local candidate
    for candidate in "$@"; do
        if have_command "$candidate"; then
            return
        fi
    done
    MISSING+=("$label (command: $(join_by '/' "$@"))")
}

need_pkg_config() {
    local label="$1"
    shift
    local module
    for module in "$@"; do
        if pkg-config --exists "$module" 2>/dev/null; then
            return
        fi
    done
    MISSING+=("$label (pkg-config: $(join_by '/' "$@"))")
}

need_header() {
    local label="$1"
    local header="$2"
    if ! printf '#include <%s>\n' "$header" | cc -E -x c - >/dev/null 2>&1; then
        MISSING+=("$label (header: $header)")
    fi
}

probe_prerequisites() {
    MISSING=()
    need_command "C compiler" cc gcc clang
    need_command "C++ compiler" c++ g++ clang++
    need_command "CMake" cmake
    need_command "Make" make
    need_command "Git" git
    need_command "pkg-config" pkg-config
    need_command "Autoconf" autoconf
    need_command "Automake" automake
    need_command "Libtool" libtoolize
    need_command "M4" m4
    need_command "Gettext" msgfmt
    need_command "Texinfo" makeinfo
    need_command "Perl" perl
    need_command "Python 3" python3
    need_command "Patch" patch
    need_command "File identification" file
    need_command "Download utility" curl wget

    if have_command pkg-config; then
        if [[ "$GTK_VERSION" == "3" ]]; then
            need_pkg_config "GTK 3 development files" gtk+-3.0
            need_pkg_config "WebKitGTK 4.1 development files" webkit2gtk-4.1
            need_pkg_config "libsoup 3 development files" libsoup-3.0
        else
            need_pkg_config "GTK 2 development files" gtk+-2.0
        fi
        need_pkg_config "D-Bus development files" dbus-1
        need_pkg_config "OpenGL development files" gl
        need_pkg_config "GLU development files" glu
        need_pkg_config "libsecret development files" libsecret-1
        need_pkg_config "libudev development files" libudev
    fi

    if have_command cc; then
        need_header "zlib development files" zlib.h
        need_header "bzip2 development files" bzlib.h
        need_header "XZ development files" lzma.h
    fi
}

bootstrap_packages() {
    case "$PACKAGE_MANAGER" in
        apt-get)
            BOOTSTRAP_PACKAGES=(
                build-essential cmake ninja-build git curl ca-certificates
                pkg-config autoconf automake libtool m4 gettext texinfo perl
                python3 patch file libdbus-1-dev libgl1-mesa-dev
                libglu1-mesa-dev libegl1-mesa-dev libsecret-1-dev libudev-dev
                zlib1g-dev libbz2-dev liblzma-dev
            )
            if [[ "$GTK_VERSION" == "3" ]]; then
                BOOTSTRAP_PACKAGES+=(libgtk-3-dev libwebkit2gtk-4.1-dev)
            else
                BOOTSTRAP_PACKAGES+=(libgtk2.0-dev)
            fi
            ;;
        dnf|yum)
            BOOTSTRAP_PACKAGES=(
                gcc gcc-c++ make cmake ninja-build git curl ca-certificates
                pkgconf-pkg-config autoconf automake libtool m4 gettext
                texinfo perl python3 patch file dbus-devel mesa-libGL-devel
                mesa-libGLU-devel mesa-libEGL-devel libsecret-devel
                systemd-devel zlib-devel bzip2-devel xz-devel
            )
            if [[ "$GTK_VERSION" == "3" ]]; then
                BOOTSTRAP_PACKAGES+=(gtk3-devel webkit2gtk4.1-devel)
            else
                BOOTSTRAP_PACKAGES+=(gtk2-devel)
            fi
            ;;
        pacman)
            BOOTSTRAP_PACKAGES=(
                base-devel cmake ninja git curl ca-certificates pkgconf
                autoconf automake libtool m4 gettext texinfo perl python
                patch file dbus mesa glu libsecret systemd zlib bzip2 xz
            )
            if [[ "$GTK_VERSION" == "3" ]]; then
                BOOTSTRAP_PACKAGES+=(gtk3 webkit2gtk-4.1)
            else
                BOOTSTRAP_PACKAGES+=(gtk2)
            fi
            ;;
        zypper)
            BOOTSTRAP_PACKAGES=(
                gcc gcc-c++ make cmake ninja git curl ca-certificates
                pkg-config autoconf automake libtool m4 gettext-tools
                texinfo perl python3 patch file dbus-1-devel
                Mesa-libGL-devel Mesa-libGLU-devel Mesa-libEGL-devel
                libsecret-devel libudev-devel zlib-devel libbz2-devel
                xz-devel
            )
            if [[ "$GTK_VERSION" == "3" ]]; then
                BOOTSTRAP_PACKAGES+=(gtk3-devel webkit2gtk3-devel)
            else
                BOOTSTRAP_PACKAGES+=(gtk2-devel)
            fi
            ;;
        apk)
            BOOTSTRAP_PACKAGES=(
                build-base cmake ninja git curl ca-certificates pkgconf
                autoconf automake libtool m4 gettext texinfo perl python3
                patch file dbus-dev mesa-dev glu-dev libsecret-dev eudev-dev
                linux-headers zlib-dev bzip2-dev xz-dev
            )
            if [[ "$GTK_VERSION" == "3" ]]; then
                BOOTSTRAP_PACKAGES+=(gtk+3.0-dev webkit2gtk-4.1-dev)
            else
                BOOTSTRAP_PACKAGES+=(gtk+2.0-dev)
            fi
            ;;
        *)
            BOOTSTRAP_PACKAGES=()
            ;;
    esac
}

as_root() {
    if ((EUID == 0)); then
        "$@"
    elif have_command sudo; then
        sudo "$@"
    else
        die "root access is required; install sudo or run the displayed package command as root"
    fi
}

install_prerequisites() {
    [[ -n "$PACKAGE_MANAGER" ]] || die "no supported package manager found; missing: $(join_by '; ' "${MISSING[@]}")"
    bootstrap_packages
    (("${#BOOTSTRAP_PACKAGES[@]}" > 0)) || die "no package map for $PACKAGE_MANAGER"

    log "Installing build prerequisites with $PACKAGE_MANAGER"
    case "$PACKAGE_MANAGER" in
        apt-get)
            as_root apt-get update
            as_root apt-get install -y "${BOOTSTRAP_PACKAGES[@]}"
            ;;
        dnf|yum)
            as_root "$PACKAGE_MANAGER" install -y "${BOOTSTRAP_PACKAGES[@]}"
            ;;
        pacman)
            as_root pacman -S --needed --noconfirm "${BOOTSTRAP_PACKAGES[@]}"
            ;;
        zypper)
            as_root zypper --non-interactive install --no-recommends "${BOOTSTRAP_PACKAGES[@]}"
            ;;
        apk)
            as_root apk add "${BOOTSTRAP_PACKAGES[@]}"
            ;;
    esac
}

probe_prerequisites
printf 'Host: %s (%s, %s)\n' "$OS_NAME" "$OS_ID" "$ARCH"
printf 'Package manager: %s\n' "${PACKAGE_MANAGER:-not detected}"

if (("${#MISSING[@]}" > 0)); then
    printf 'Missing prerequisites:\n' >&2
    printf '  - %s\n' "${MISSING[@]}" >&2
    if ((INSTALL_MISSING)); then
        install_prerequisites
        probe_prerequisites
    else
        printf '\nRun ./BuildLinux.sh --install to install the mapped packages.\n' >&2
        exit 2
    fi
fi

(("${#MISSING[@]}" == 0)) || die "prerequisites are still missing after package installation: $(join_by '; ' "${MISSING[@]}")"
printf 'Prerequisites: ready\n'

if ((CHECK_ONLY)); then
    exit 0
fi

if ((INSTALL_MISSING && option_selected == 0)); then
    exit 0
fi

if [[ -z "$JOBS" ]]; then
    CPU_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || printf '1')"
    MEM_KB="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || printf '0')"
    if [[ "$MEM_KB" =~ ^[0-9]+$ ]] && ((MEM_KB > 0)); then
        MEM_JOBS=$((MEM_KB / 2097152))
        ((MEM_JOBS < 1)) && MEM_JOBS=1
        ((MEM_JOBS < CPU_JOBS)) && CPU_JOBS="$MEM_JOBS"
    fi
    ((CPU_JOBS > 8)) && CPU_JOBS=8
    JOBS="$CPU_JOBS"
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "job count must be a positive integer"

AVAILABLE_KB="$(df -Pk "$ROOT_DIR" | awk 'NR == 2 {print $4}')"
MINIMUM_KB=$((10 * 1024 * 1024))
((AVAILABLE_KB >= MINIMUM_KB)) || die "at least 10 GiB of free disk space is required"

BUILD_FLAVOR="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
DEPS_BUILD_DIR="$ROOT_DIR/deps/build-linux-$ARCH-$BUILD_FLAVOR"
SLIC3R_BUILD_DIR="$ROOT_DIR/build-linux-$ARCH-$BUILD_FLAVOR"
DEPS_PREFIX="$DEPS_BUILD_DIR/destdir/usr/local"

if have_command ninja; then
    GENERATOR="Ninja"
else
    GENERATOR="Unix Makefiles"
fi

export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"

if ((WIPE)); then
    log "Removing selected generated build directories"
    ((BUILD_DEPS)) && cmake -E remove_directory "$DEPS_BUILD_DIR"
    ((BUILD_SLIC3R)) && cmake -E remove_directory "$SLIC3R_BUILD_DIR"
fi

if ((CLEAN_DEP_STAMPS)) && [[ -d "$DEPS_BUILD_DIR" ]]; then
    log "Removing dependency stamps"
    find "$DEPS_BUILD_DIR" -path '*/src/*-stamp/*' -type f -delete
fi

GTK_CMAKE="ON"
[[ "$GTK_VERSION" == "2" ]] && GTK_CMAKE="OFF"

if ((BUILD_DEPS)); then
    log "Configuring bundled dependencies in $DEPS_BUILD_DIR"
    cmake -S "$ROOT_DIR/deps" -B "$DEPS_BUILD_DIR" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DDEP_WX_GTK3="$GTK_CMAKE" \
        -DDEP_MAX_THREADS="$JOBS" \
        -DDEP_DOWNLOAD_DIR="$ROOT_DIR/deps/.pkg_cache"

    if ((CONFIGURE_ONLY == 0)); then
        log "Building bundled dependencies"
        # The dependency graph manages parallelism inside individual projects.
        cmake --build "$DEPS_BUILD_DIR" --parallel 1
    fi
fi

if ((BUILD_SLIC3R)); then
    if [[ ! -f "$DEPS_PREFIX/include/boost/version.hpp" ]]; then
        die "bundled dependencies are incomplete; rerun with --deps --slicer"
    fi

    if git submodule status resources/profiles 2>/dev/null | grep -q '^-'; then
        log "Initializing profile submodule"
        git submodule update --init --recursive resources/profiles
    fi

    if ((VERSION_DATE)); then
        sed "s/+UNKNOWN/-$(date '+%F')/" "$ROOT_DIR/version.inc" > "$ROOT_DIR/version.date.inc"
    fi

    TESTS_CMAKE="OFF"
    ((BUILD_TESTS)) && TESTS_CMAKE="ON"

    log "Configuring SuperSlicer in $SLIC3R_BUILD_DIR"
    cmake -S "$ROOT_DIR" -B "$SLIC3R_BUILD_DIR" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DSLIC3R_STATIC=ON \
        -DSLIC3R_GTK="$GTK_VERSION" \
        -DSLIC3R_BUILD_TESTS="$TESTS_CMAKE"

    if ((CONFIGURE_ONLY == 0)); then
        log "Building SuperSlicer"
        cmake --build "$SLIC3R_BUILD_DIR" --target Slic3r OCCTWrapper --parallel "$JOBS"

        if ((UPDATE_POT)); then
            cmake --build "$SLIC3R_BUILD_DIR" --target gettext_make_pot --parallel "$JOBS"
        fi
        cmake --build "$SLIC3R_BUILD_DIR" --target gettext_po_to_mo --parallel "$JOBS"

        # Give editor and command-line launchers a stable path across CPU architectures.
        ln -sfn "$SLIC3R_BUILD_DIR" "$ROOT_DIR/build-linux-current-$BUILD_FLAVOR"

        if ((BUILD_PACKAGE)); then
            IMAGE_SCRIPT="$SLIC3R_BUILD_DIR/src/BuildLinuxImage.sh"
            [[ -x "$IMAGE_SCRIPT" ]] || chmod 755 "$IMAGE_SCRIPT"
            (
                cd "$SLIC3R_BUILD_DIR"
                "$IMAGE_SCRIPT" -a
                "$IMAGE_SCRIPT" -i
            )
        fi
    fi
fi

log "Build completed"
printf 'Dependencies: %s\n' "$DEPS_BUILD_DIR"
printf 'SuperSlicer:  %s\n' "$SLIC3R_BUILD_DIR"
