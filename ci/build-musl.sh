#!/bin/sh
#
# ci/build-musl.sh — static musl build of microsnander for one Linux target
#
#   ci/build-musl.sh <target> [outdir]
#
# target: x86_64   Bootlin x86-64 musl toolchain — PCs and servers
#         arm      OpenIPC toolchain (hisilicon-hi3516cv100, arm musleabi, ARMv5TE
#                  soft-float) — runs on every 32-bit ARM camera and board
#
# Produces <outdir>/microsnander-linux-<target>: static, stripped, no version in the
# name, so the release links never change. libusb is built from its release tarball
# for the target, without udev (devices are found by scanning /dev/bus/usb), so the
# binary has no dependencies and runs whatever the libc of the system. Any compiler
# warning in microsnander fails the build. The binary must start and print the
# version that src/main.c defines (under qemu-user for arm).
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

LIBUSB_VERSION=1.0.30
LIBUSB_SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
LIBUSB_URL=https://github.com/libusb/libusb/releases/download/v$LIBUSB_VERSION/libusb-$LIBUSB_VERSION.tar.bz2

OPENIPC=https://github.com/OpenIPC/firmware/releases/download/toolchain
BOOTLIN=https://toolchains.bootlin.com/downloads/releases/toolchains
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
    *) echo "usage: $0 <x86_64|arm> [outdir]" >&2; exit 2 ;;
esac

# Download first (with retries), then check, then extract: a truncated stream piped
# into tar looks like a corrupt archive.
fetch() {   # url sha256 file
    curl -fsSL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c - >/dev/null ||
        { echo "SHA256 mismatch for $1" >&2; rm -f "$3"; exit 1; }
}

DIR=$CACHE/$DIRNAME
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
CROSS=${GCC%gcc}
HOST=$(basename "${CROSS%-}")
# configure looks for <host>-ar, <host>-ranlib, <host>-strip in PATH
PATH=$DIR/bin:$PATH
export PATH

BUILD=$ROOT/build/musl-$ARCH
rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT"

# libusb for the target: static only, no udev.
# ac_cv_header_linux_netlink_h=yes: configure checks <linux/netlink.h> on its own,
# and the Linux 3.0 headers of the OpenIPC toolchain need <sys/socket.h> first
# (sa_family_t); libusb itself includes them in that order (os/linux_netlink.c).
echo "building libusb $LIBUSB_VERSION for $HOST"
fetch "$LIBUSB_URL" "$LIBUSB_SHA" "$BUILD/libusb.tar.bz2"
tar -xjf "$BUILD/libusb.tar.bz2" -C "$BUILD"
(
    cd "$BUILD/libusb-$LIBUSB_VERSION"
    ./configure -q --host="$HOST" --prefix="$BUILD/libusb" CC="${CROSS}gcc" \
        --enable-static --disable-shared --disable-udev --disable-examples-build \
        --disable-tests-build ac_cv_header_linux_netlink_h=yes \
        > "$BUILD/libusb-configure.log" 2>&1 ||
        { cat "$BUILD/libusb-configure.log"; exit 1; }
    make -s -j"$(nproc)" install > "$BUILD/libusb-build.log" 2>&1 ||
        { cat "$BUILD/libusb-build.log"; exit 1; }
)

# microsnander: objects of another toolchain are never rebuilt by make, so clean first
make -s -C "$ROOT/src" clean
# No pipe: a failing make must stop the script (sh has no pipefail).
if ! make -C "$ROOT/src" CC="${CROSS}gcc" LDFLAGS=-static \
        LIBUSB_CFLAGS="-I$BUILD/libusb/include" \
        LIBUSB_LIBS="$BUILD/libusb/lib/libusb-1.0.a" > "$BUILD/build.log" 2>&1; then
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
