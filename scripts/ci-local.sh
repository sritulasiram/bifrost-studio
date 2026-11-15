#!/bin/sh
# Mirror .github/workflows/ci.yml on a Mac (E-4).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BIFROST_CI_BUILD:-$HOME/Library/Caches/BifrostBuild-ci}"
LIBUSB_VERSION="${LIBUSB_VERSION:-1.0.29}"
LIBUSB_SHA256="${LIBUSB_SHA256:-5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85}"

if ! command -v cmake >/dev/null; then
    echo "cmake not found; install via: brew install cmake" >&2
    exit 1
fi

if [ ! -f "$HOME/.local/lib/libusb-1.0.0.dylib" ]; then
    echo "Building libusb into ~/.local..."
    WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK"' EXIT
    curl -fsSL -o "$WORK/libusb.tar.bz2" \
        "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2"
    echo "${LIBUSB_SHA256}  $WORK/libusb.tar.bz2" | shasum -a 256 -c -
    tar xjf "$WORK/libusb.tar.bz2" -C "$WORK"
    cd "$WORK/libusb-${LIBUSB_VERSION}"
    ./configure --enable-static --enable-shared --prefix="$HOME/.local" \
        CFLAGS="-arch arm64 -arch x86_64 -mmacosx-version-min=13.0 -O2"
    make -j"$(sysctl -n hw.ncpu)"
    make install
fi

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DLIBUSB_ROOT="$HOME/.local"
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)"
chmod +x "$ROOT/scripts/check-l10n.sh"
"$ROOT/scripts/check-l10n.sh"
ctest --test-dir "$BUILD" --output-on-failure
echo "CI local: OK ($BUILD)"