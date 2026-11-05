#!/bin/sh
#
# ci/build-musl.sh — static musl build of microsnander for one Linux target
#
#   ci/build-musl.sh <target> [outdir]
#
# target: x86_64   Bootlin x86-64 musl toolchain — PCs and servers
#         arm      OpenIPC toolchain (hisilicon-hi3516cv100, arm musleabi, ARMv5TE
#                  soft-float) — runs on every 32-bit ARM camera and board
#         mipsel   OpenWrt toolchain (ramips/mt7621, musl, soft-float) — runs on every
#                  little-endian MIPS32r2 core with or without an FPU: MediaTek routers,
#                  Ingenic cameras
#
# Produces <outdir>/microsnander-linux-<target>: static, stripped, no version in the
# name, so the release links never change. The programmers are reached through the
# Linux usbfs (/dev/bus/usb) by src/usbfs.c, without libusb, so the binary has no
# dependencies and runs whatever the libc of the system. Any compiler
# warning in microsnander fails the build. The binary must start and print the
# version that src/main.c defines (under qemu-user for arm and mipsel).
#
# Toolchains are downloaded once into $TOOLCHAIN_CACHE and checked by SHA256 before
# their compiler is run; the OpenIPC toolchain is a rolling release asset, checked
# against the digest GitHub reports for it.
set -eu

ARCH=${1:-}
OUT=${2:-dist}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${TOOLCHAIN_CACHE:-$HOME/.cache/microsnander-toolchains}
# Relative paths are the caller's: the build below runs elsewhere.
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT ;; esac
case "$CACHE" in /*) ;; *) CACHE=$PWD/$CACHE ;; esac

OPENIPC=https://github.com/OpenIPC/firmware/releases/download/toolchain
BOOTLIN=https://toolchains.bootlin.com/downloads/releases/toolchains
OPENWRT=https://downloads.openwrt.org/releases/23.05.5/targets
SUBDIR=     # toolchain directory inside the archive, if not its top level
case "$ARCH" in
    x86_64)
        DIRNAME=x86-64--musl--stable-2026.08-1
        URL=$BOOTLIN/x86-64/tarballs/$DIRNAME.tar.xz
        SHA=78d3a4683d6ac47b5ee73bd5bce210b55eb93dff1b137c61298af97eb0d2b5a6
        RUN= ;;
    arm)
        DIRNAME=arm-openipc-linux-musleabi_sdk-buildroot
        URL=$OPENIPC/toolchain.hisilicon-hi3516cv100.tgz
        SHA=github
        RUN=qemu-arm ;;
    mipsel)
        DIRNAME=openwrt-toolchain-23.05.5-ramips-mt7621_gcc-12.3.0_musl.Linux-x86_64
        URL=$OPENWRT/ramips/mt7621/$DIRNAME.tar.xz
        SHA=bb08478a46869009a2a8b85f88e266cc6b6d3ccefc34de61907ead2a986e08c9
        SUBDIR=/toolchain-mipsel_24kc_gcc-12.3.0_musl
        RUN=qemu-mipsel ;;
    *) echo "usage: $0 <x86_64|arm|mipsel> [outdir]" >&2; exit 2 ;;
esac

# Download first (with retries), then check, then extract: a truncated stream piped
# into tar looks like a corrupt archive.
fetch() {   # url sha256 file
    curl -fsSL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c - >/dev/null ||
        { echo "SHA256 mismatch for $1" >&2; rm -f "$3"; exit 1; }
}

DIR=$CACHE/$DIRNAME$SUBDIR
if [ ! -d "$DIR/bin" ]; then
    mkdir -p "$CACHE"
    echo "downloading $URL"
    if [ "$SHA" = github ]; then
        # authenticated when a token is available: anonymous calls share a 60/hour limit per IP
        TOKEN=${GITHUB_TOKEN:-${GH_TOKEN:-}}
        REL=$(curl -fsSL --retry 3 ${TOKEN:+-H "Authorization: Bearer $TOKEN"} \
              "https://api.github.com/repos/OpenIPC/firmware/releases/tags/toolchain") ||
            { echo "GitHub API request for the toolchain digest failed (rate limit?)" >&2; exit 1; }
        SHA=$(printf '%s' "$REL" | jq -r --arg n "${URL##*/}" \
              '.assets[] | select(.name == $n) | .digest // "" | sub("^sha256:"; "")')
        [ -n "$SHA" ] || { echo "no digest for ${URL##*/} in the toolchain release" >&2; exit 1; }
    fi
    fetch "$URL" "$SHA" "$CACHE/toolchain.tar"
    tar -xf "$CACHE/toolchain.tar" -C "$CACHE"
    rm -f "$CACHE/toolchain.tar"
    if [ -x "$DIR/relocate-sdk.sh" ]; then "$DIR/relocate-sdk.sh" >/dev/null; fi
fi
# An empty CROSS would build with the host compiler: a glibc binary
GCC=$(ls "$DIR"/bin/*-linux-musl*-gcc 2>/dev/null | head -1)
[ -n "$GCC" ] || { echo "no musl gcc in $DIR/bin: remove $DIR and run again" >&2; exit 1; }
export STAGING_DIR="$DIR"     # OpenWrt's gcc wrapper warns without it
CROSS=${GCC%gcc}
HOST=$(basename "${CROSS%-}")
# configure looks for <host>-ar, <host>-ranlib, <host>-strip in PATH
PATH=$DIR/bin:$PATH
export PATH

BUILD=$ROOT/build/musl-$ARCH
rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT"

# microsnander: objects of another toolchain are never rebuilt by make, so clean first
make -s -C "$ROOT/src" clean
# No pipe: a failing make must stop the script (sh has no pipefail).
if ! make -C "$ROOT/src" CC="${CROSS}gcc" LDFLAGS=-static > "$BUILD/build.log" 2>&1; then
    cat "$BUILD/build.log"
    exit 1
fi
cat "$BUILD/build.log"
if grep -q 'warning:' "$BUILD/build.log"; then
    echo "compiler warnings, see above" >&2
    exit 1
fi
BIN=$OUT/microsnander-linux-$ARCH
mv "$ROOT/src/microsnander" "$BIN"
make -s -C "$ROOT/src" clean

# A static binary has no program interpreter (ld-linux*.so / ld-musl*.so)
if "${CROSS}readelf" -l "$BIN" | grep -q 'Requesting program interpreter'; then
    echo "$BIN is dynamically linked" >&2; exit 1
fi
echo "microsnander-linux-$ARCH: $(wc -c < "$BIN") bytes, static"

# The binary must start and print the version src/main.c defines (-h exits 0
# without opening the programmer).
ver=$(sed -n 's/^#define[[:space:]]*_VER[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/src/main.c")
[ -n "$ver" ] || { echo "no _VER in src/main.c" >&2; exit 1; }
banner=$($RUN "$BIN" -h | grep -m1 ' v\.') || { echo "$BIN -h failed" >&2; exit 1; }
echo "$banner"
case "$banner" in
    *" v.$ver") ;;
    *) echo "the binary does not print v.$ver" >&2; exit 1 ;;
esac
