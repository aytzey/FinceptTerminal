#!/usr/bin/env bash
set -euo pipefail

# ── Parse args ──────────────────────────────────────────────
CI_MODE=false
ALLOW_QT_DRIFT=false
for arg in "$@"; do
    case "$arg" in
        --ci) CI_MODE=true ;;
        --allow-qt-drift) ALLOW_QT_DRIFT=true ;;
    esac
done

# ── Pinned versions (must match CMakeLists.txt) ─────────────
QT_VERSION="6.8.3"
PYTHON_MIN="3.11"
CMAKE_MIN="3.27"
GCC_MIN="12.3"
CLANG_MIN="15.0"

# ── Colours ─────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "  ${GREEN}OK${NC}"; }
fail() { echo -e "  ${RED}ERROR: $1${NC}"; exit 1; }
info() { echo -e "  ${YELLOW}$1${NC}"; }

echo ""
echo "================================================"
echo "  Fincept Terminal v4.0.2 — Setup"
echo "  Pinned: Qt ${QT_VERSION} | CMake ${CMAKE_MIN}+ | Python ${PYTHON_MIN}+"
[ "$CI_MODE" = true ] && echo "  (CI mode — skipping interactive steps)"
echo "================================================"
echo ""

# ── Detect OS ───────────────────────────────────────────────
OS="$(uname -s)"
case "$OS" in
    Linux*)  PLATFORM="linux" ; QT_KIT="gcc_64"     ; PRESET="linux-release" ;;
    Darwin*) PLATFORM="macos" ; QT_KIT="clang_64"   ; PRESET="macos-release" ;;
    *)       fail "Unsupported OS: $OS" ;;
esac
echo "Platform: $OS"
echo ""

# ── Helper: version >= min ──────────────────────────────────
version_ge() {
    # $1=actual  $2=min    returns 0 (true) if actual >= min
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]
}

escape_sed_replacement() {
    printf '%s' "$1" | sed 's/[\/&]/\\&/g'
}

normalize_qt_prefix() {
    local candidate="$1"
    if [ -f "$candidate/lib/cmake/Qt6/Qt6Config.cmake" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi
    if [ -f "$candidate/cmake/Qt6/Qt6Config.cmake" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi
    if [ -f "$candidate/Qt6Config.cmake" ]; then
        case "$candidate" in
            */lib/cmake/Qt6) printf '%s\n' "${candidate%/lib/cmake/Qt6}" ;;
            */cmake/Qt6) printf '%s\n' "${candidate%/cmake/Qt6}" ;;
            *) printf '%s\n' "$candidate" ;;
        esac
        return 0
    fi
    return 1
}

install_linux_dev_integration() {
    local dev_dir="$APP_DIR/packaging/linux/dev"
    local bin_dir="$HOME/.local/bin"
    local app_dir="$HOME/.local/share/applications"
    local systemd_dir="$HOME/.config/systemd/user"
    local launcher_path="$bin_dir/fincept-terminal-dev"
    local watcher_path="$bin_dir/fincept-terminal-dev-watch"
    local desktop_path="$app_dir/fincept-terminal-dev.desktop"
    local service_path="$systemd_dir/fincept-terminal-dev.service"
    local icon_path="$SCRIPT_DIR/fincept_icon.ico"
    local extra_cmake_args=""

    [ "$ALLOW_QT_DRIFT" = true ] && extra_cmake_args="-DFINCEPT_ALLOW_QT_DRIFT=ON"

    mkdir -p "$bin_dir" "$app_dir" "$systemd_dir"

    local app_dir_escaped qt_prefix_escaped extra_args_escaped launcher_path_escaped watcher_path_escaped icon_path_escaped
    app_dir_escaped="$(escape_sed_replacement "$APP_DIR")"
    qt_prefix_escaped="$(escape_sed_replacement "$QT_PREFIX")"
    extra_args_escaped="$(escape_sed_replacement "$extra_cmake_args")"
    launcher_path_escaped="$(escape_sed_replacement "$launcher_path")"
    watcher_path_escaped="$(escape_sed_replacement "$watcher_path")"
    icon_path_escaped="$(escape_sed_replacement "$icon_path")"

    sed \
        -e "s/__APP_DIR__/$app_dir_escaped/g" \
        -e "s/__CMAKE_PREFIX_PATH__/$qt_prefix_escaped/g" \
        -e "s/__EXTRA_CMAKE_ARGS__/$extra_args_escaped/g" \
        "$dev_dir/fincept-terminal-dev-launch.sh.in" > "$launcher_path"
    chmod 755 "$launcher_path"

    sed \
        -e "s/__APP_DIR__/$app_dir_escaped/g" \
        -e "s/__CMAKE_PREFIX_PATH__/$qt_prefix_escaped/g" \
        -e "s/__EXTRA_CMAKE_ARGS__/$extra_args_escaped/g" \
        "$dev_dir/fincept-terminal-dev-watch.sh.in" > "$watcher_path"
    chmod 755 "$watcher_path"

    sed \
        -e "s/__APP_DIR__/$app_dir_escaped/g" \
        -e "s/__WATCH_SCRIPT_PATH__/$watcher_path_escaped/g" \
        "$dev_dir/fincept-terminal-dev.service.in" > "$service_path"

    sed \
        -e "s/__APP_DIR__/$app_dir_escaped/g" \
        -e "s/__LAUNCH_SCRIPT_PATH__/$launcher_path_escaped/g" \
        -e "s/__ICON_PATH__/$icon_path_escaped/g" \
        "$dev_dir/fincept-terminal-dev.desktop.in" > "$desktop_path"

    if command -v update-desktop-database &>/dev/null; then
        update-desktop-database "$app_dir" >/dev/null 2>&1 || true
    fi

    if command -v systemctl &>/dev/null && systemctl --user show-environment &>/dev/null; then
        systemctl --user daemon-reload
        systemctl --user enable --now fincept-terminal-dev.service >/dev/null
        info "Installed Start menu launcher: Fincept Terminal Dev"
        info "Enabled auto-builder service: fincept-terminal-dev.service"
    else
        info "Installed launcher files, but could not enable systemd --user service in this session."
    fi
}

TOTAL_STEPS=8
if [ "$PLATFORM" = "linux" ] && [ "$CI_MODE" = false ]; then
    TOTAL_STEPS=9
fi

# ── Step 1: System dependencies (build tools only) ──────────
echo "[1/${TOTAL_STEPS}] Installing system build tools..."
if [ "$PLATFORM" = "linux" ]; then
    command -v apt-get &>/dev/null || fail "apt-get not found. Install cmake / ninja-build / g++ / python3.11 / python3-pip manually."
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends \
        git cmake ninja-build g++ \
        inotify-tools \
        python3 python3-pip python3-venv \
        libgl1-mesa-dev libglu1-mesa-dev \
        libxkbcommon-dev libxkbcommon-x11-dev \
        libfontconfig1 libdbus-1-3 \
        pkg-config curl
elif [ "$PLATFORM" = "macos" ]; then
    if ! command -v brew &>/dev/null; then
        [ "$CI_MODE" = true ] && fail "Homebrew not found in CI environment."
        info "Homebrew not found. Installing..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
    brew install cmake ninja python@3.11
fi
ok

# ── Step 2: Verify compiler version ─────────────────────────
echo "[2/${TOTAL_STEPS}] Checking C++ compiler..."
if [ "$PLATFORM" = "linux" ]; then
    command -v g++ &>/dev/null || fail "g++ not found."
    GCC_VER="$(g++ -dumpfullversion -dumpversion 2>/dev/null || g++ --version | head -1 | awk '{print $NF}')"
    echo "  g++ ${GCC_VER}"
    version_ge "$GCC_VER" "$GCC_MIN" || fail "GCC ${GCC_MIN}+ required. Found ${GCC_VER}. Install g++-12 or newer."
elif [ "$PLATFORM" = "macos" ]; then
    command -v clang++ &>/dev/null || fail "clang++ not found. Run: xcode-select --install"
    CLANG_VER="$(clang++ --version | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    echo "  Apple Clang ${CLANG_VER}"
    version_ge "$CLANG_VER" "$CLANG_MIN" || fail "Apple Clang ${CLANG_MIN}+ required (Xcode 15.2+). Found ${CLANG_VER}."
fi
ok

# ── Step 3: Verify CMake version ────────────────────────────
echo "[3/${TOTAL_STEPS}] Checking CMake..."
command -v cmake &>/dev/null || fail "cmake not found."
CMAKE_VER="$(cmake --version | head -1 | awk '{print $3}')"
echo "  cmake ${CMAKE_VER}"
version_ge "$CMAKE_VER" "$CMAKE_MIN" || fail "CMake ${CMAKE_MIN}+ required. Found ${CMAKE_VER}. Download from https://cmake.org/download/"
ok

# ── Step 4: Verify Python version ───────────────────────────
echo "[4/${TOTAL_STEPS}] Checking Python..."
PYTHON="$(command -v python3.11 || command -v python3 || true)"
[ -n "$PYTHON" ] || fail "python3 not found."
PY_VER="$($PYTHON -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])')"
echo "  python ${PY_VER}"
version_ge "$PY_VER" "$PYTHON_MIN" || fail "Python ${PYTHON_MIN}+ required. Found ${PY_VER}."
ok

# ── Step 5: Install pinned Qt ${QT_VERSION} via aqtinstall ──
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_INSTALL_ROOT="${FINCEPT_QT_ROOT:-$SCRIPT_DIR/.qt}"
QT_PREFIX="$QT_INSTALL_ROOT/$QT_VERSION/$QT_KIT"

echo "[5/${TOTAL_STEPS}] Locating Qt ${QT_VERSION}..."
if [ -n "${Qt6_DIR:-}" ] && QT_ENV_PREFIX="$(normalize_qt_prefix "$Qt6_DIR")"; then
    QT_PREFIX="$QT_ENV_PREFIX"
    info "Using Qt from Qt6_DIR env: $QT_PREFIX"
elif [ -f "$QT_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ]; then
    info "Qt ${QT_VERSION} already installed at $QT_PREFIX"
else
    info "Installing Qt ${QT_VERSION} via aqtinstall to $QT_INSTALL_ROOT ..."
    # aqtinstall is a stable community tool that downloads exact Qt versions
    # from the official Qt mirror. Much smaller than Qt Online Installer and scriptable.
    AQT_VENV="$SCRIPT_DIR/.aqt-venv"
    [ -x "$AQT_VENV/bin/python" ] || "$PYTHON" -m venv "$AQT_VENV"
    "$AQT_VENV/bin/python" -m pip install --quiet --upgrade pip aqtinstall
    "$AQT_VENV/bin/python" -m aqt help >/dev/null 2>&1 || fail "aqtinstall did not install correctly."
    AQT="$AQT_VENV/bin/python -m aqt"
    # Qt host/target/arch
    if [ "$PLATFORM" = "linux" ]; then
        AQT_HOST="linux"   ; AQT_TARGET="desktop" ; AQT_ARCH="gcc_64"
    else
        AQT_HOST="mac"     ; AQT_TARGET="desktop" ; AQT_ARCH="clang_64"
    fi
    # Modules required to compile Fincept (match find_package COMPONENTS)
    AQT_MODULES="qtcharts qtwebsockets qtmultimedia qtspeech"
    $AQT install-qt "$AQT_HOST" "$AQT_TARGET" "$QT_VERSION" "$AQT_ARCH" \
        --outputdir "$QT_INSTALL_ROOT" \
        --modules $AQT_MODULES \
        || fail "aqtinstall failed. Check internet connection or install Qt ${QT_VERSION} manually from https://www.qt.io/download-qt-installer"
    [ -f "$QT_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ] || fail "Qt install completed but Qt6Config.cmake not found at $QT_PREFIX"
fi
export CMAKE_PREFIX_PATH="$QT_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
echo "  CMAKE_PREFIX_PATH=$QT_PREFIX"
ok

# ── Step 6: Configure (using CMake preset) ──────────────────
APP_DIR="$SCRIPT_DIR/fincept-qt"
[ -d "$APP_DIR" ] || fail "fincept-qt directory not found. Ensure you cloned the full repository."
cd "$APP_DIR"

echo "[6/${TOTAL_STEPS}] Configuring (preset: $PRESET)..."
# Override the preset's default CMAKE_PREFIX_PATH with the one we just set,
# so the build picks up the aqtinstall location rather than ~/Qt/6.8.3/...
CONFIGURE_ARGS=(--preset "$PRESET" "-DCMAKE_PREFIX_PATH=$QT_PREFIX")
[ "$ALLOW_QT_DRIFT" = true ] && CONFIGURE_ARGS+=(-DFINCEPT_ALLOW_QT_DRIFT=ON)
cmake "${CONFIGURE_ARGS[@]}" \
    || fail "CMake configure failed. See error above."
ok

# ── Step 7: Build ───────────────────────────────────────────
echo "[7/${TOTAL_STEPS}] Compiling..."
cmake --build --preset "$PRESET" || fail "Build failed. See error above."
ok

# ── Step 8: Python runtime env ──────────────────────────────
echo "[8/${TOTAL_STEPS}] Preparing Python runtime..."
RUNTIME_VENV="$APP_DIR/build/$PRESET/venv-numpy2"
[ -x "$RUNTIME_VENV/bin/python3" ] || "$PYTHON" -m venv "$RUNTIME_VENV"
"$RUNTIME_VENV/bin/python3" -m pip install --quiet --upgrade pip
"$RUNTIME_VENV/bin/python3" -m pip install --quiet yfinance requests beautifulsoup4 \
    || fail "Python runtime dependency install failed."
ok

if [ "$PLATFORM" = "linux" ] && [ "$CI_MODE" = false ]; then
    echo "[9/${TOTAL_STEPS}] Installing Linux development launcher..."
    install_linux_dev_integration
    ok
fi

# ── Done ────────────────────────────────────────────────────
BIN="$APP_DIR/build/$PRESET/FinceptTerminal"
[ "$PLATFORM" = "macos" ] && BIN="$APP_DIR/build/$PRESET/FinceptTerminal.app/Contents/MacOS/FinceptTerminal"

echo ""
echo "================================================"
echo "  Build complete!"
echo "  Run: $BIN"
[ "$PLATFORM" = "linux" ] && [ "$CI_MODE" = false ] && echo "  Menu: Fincept Terminal Dev"
[ "$PLATFORM" = "linux" ] && [ "$CI_MODE" = false ] && echo "  Dev CLI: $HOME/.local/bin/fincept-terminal-dev"
echo "================================================"
echo ""

if [ "$CI_MODE" = true ]; then
    exit 0
fi

read -r -p "  Launch now? (y/n): " LAUNCH
if [[ "$LAUNCH" =~ ^[Yy]$ ]]; then
    "$BIN"
fi
