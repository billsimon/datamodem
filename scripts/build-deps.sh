#!/bin/sh
# Build the libraries a release binary links statically - OpenSSL, spandsp
# and pjproject - into one prefix, so the binary carries them and runs on a
# machine that has none of them installed.
#
#   scripts/build-deps.sh <prefix>
#   cmake -S . -B build -DDATAMODEM_STATIC_DEPS=ON -DCMAKE_PREFIX_PATH=<prefix>
#
# Static archives only: with no shared library in the prefix, the linker has
# nothing to choose but the archive. Each library leaves a stamp in the
# prefix when it is installed, and a rerun skips the ones already there. Sources are fetched by checksum into
# $DEPS_DOWNLOADS (default <prefix>/../deps-src). Needs a C compiler, make,
# perl, pkg-config and libtiff's headers; on Linux, ALSA's.
set -eu

OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
# soft-switch.org no longer serves the 0.0.6 tarball. Debian's orig tarball
# is the same release less some test images, under a stable URL.
SPANDSP_VERSION=0.0.6
SPANDSP_URL=https://deb.debian.org/debian/pool/main/s/spandsp/spandsp_0.0.6+dfsg.orig.tar.xz
SPANDSP_SHA256=3dcdc611b8a119f1f26540d05e6279c4c1e5cd576271f6d45df431359fc190f9
PJPROJECT_VERSION=2.17
PJPROJECT_SHA256=065fe06c06788d97c35f563796d59f00ce52fe9558a52d7b490a042a966facce

if [ $# -ne 1 ]; then
    echo "usage: $0 <prefix>" >&2
    exit 2
fi
mkdir -p "$1"
PREFIX=$(cd "$1" && pwd)
DL=${DEPS_DOWNLOADS:-$(dirname "$PREFIX")/deps-src}
mkdir -p "$DL"
DL=$(cd "$DL" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/datamodem-deps.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux) OS=linux ;;
    CYGWIN*) OS=cygwin ;;
    *) echo "unsupported build host: $(uname -s)" >&2; exit 1 ;;
esac

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
export CFLAGS="${CFLAGS:--O2}"
# datamodem links as a PIE on Linux. Ubuntu's GCC makes everything
# position-independent anyway; RHEL's, and the manylinux image's, do not.
if [ "$OS" = linux ]; then
    CFLAGS="$CFLAGS -fPIC"
fi
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# fetch <url> <file> <sha256>
fetch() {
    if [ ! -f "$DL/$2" ] || [ "$(sha256 "$DL/$2")" != "$3" ]; then
        echo "==> fetching $1"
        curl -fsSL --retry 3 -o "$DL/$2.part" "$1"
        mv "$DL/$2.part" "$DL/$2"
    fi
    got=$(sha256 "$DL/$2")
    if [ "$got" != "$3" ]; then
        echo "$2: sha256 $got, expected $3" >&2
        exit 1
    fi
}

# unpack <file> <name> - into $WORK/<name>, printing that directory
unpack() {
    mkdir "$WORK/$2"
    tar -xf "$DL/$1" -C "$WORK/$2" --strip-components=1
    echo "$WORK/$2"
}

# built <name> <version> - whether a previous run already installed it
built() {
    [ -f "$PREFIX/.built-$1-$2" ]
}

# --- OpenSSL --------------------------------------------------------------
# pjproject's TLS transport (--transport tls) is built on it.
if built openssl "$OPENSSL_VERSION"; then
    echo "==> OpenSSL $OPENSSL_VERSION already built"
else
fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" \
    "openssl-$OPENSSL_VERSION.tar.gz" "$OPENSSL_SHA256"
src=$(unpack "openssl-$OPENSSL_VERSION.tar.gz" openssl)
echo "==> building OpenSSL $OPENSSL_VERSION"
(
    cd "$src"
    # --libdir=lib: not lib64, which nothing else here would look in.
    ./Configure no-shared no-module no-tests no-docs \
        --prefix="$PREFIX" --libdir=lib --openssldir=/etc/ssl
    make -j"$JOBS" build_libs >"$WORK/openssl.log" 2>&1 || { tail -50 "$WORK/openssl.log"; exit 1; }
    make install_dev >/dev/null
    mkdir -p "$PREFIX/share/licenses/openssl"
    cp LICENSE.txt "$PREFIX/share/licenses/openssl/"
)
touch "$PREFIX/.built-openssl-$OPENSSL_VERSION"
fi

# --- spandsp --------------------------------------------------------------
fetch "https://github.com/pjsip/pjproject/archive/refs/tags/$PJPROJECT_VERSION.tar.gz" \
    "pjproject-$PJPROJECT_VERSION.tar.gz" "$PJPROJECT_SHA256"
pjsrc=$(unpack "pjproject-$PJPROJECT_VERSION.tar.gz" pjproject)
if built spandsp "$SPANDSP_VERSION"; then
    echo "==> spandsp $SPANDSP_VERSION already built"
else
fetch "$SPANDSP_URL" "spandsp-$SPANDSP_VERSION.tar.xz" "$SPANDSP_SHA256"
src=$(unpack "spandsp-$SPANDSP_VERSION.tar.xz" spandsp)
echo "==> building spandsp $SPANDSP_VERSION"
(
    cd "$src"
    # Its config.guess is from 2008 and has never heard of aarch64;
    # pjproject's is from 2024.
    cp "$pjsrc/config.guess" "$pjsrc/config.sub" config/
    # spandsp.h includes <tiffio.h>, and configure insists on finding libtiff
    # even though the parts of spandsp datamodem uses never call it.
    ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-shared --enable-static \
        CPPFLAGS="$(pkg-config --cflags libtiff-4)" \
        LDFLAGS="$(pkg-config --libs-only-L libtiff-4)" >"$WORK/spandsp.log" 2>&1 \
        || { tail -50 "$WORK/spandsp.log"; cat config.log | tail -50; exit 1; }
    # The table generators run on the build host mid-build; serially, as
    # Homebrew and Debian both build it.
    make >>"$WORK/spandsp.log" 2>&1 || { tail -50 "$WORK/spandsp.log"; exit 1; }
    make install >/dev/null
    mkdir -p "$PREFIX/share/licenses/spandsp"
    cp COPYING "$PREFIX/share/licenses/spandsp/"
)
touch "$PREFIX/.built-spandsp-$SPANDSP_VERSION"
fi

# --- pjproject ------------------------------------------------------------
if built pjproject "$PJPROJECT_VERSION"; then
    echo "==> pjproject $PJPROJECT_VERSION already built"
else
echo "==> building pjproject $PJPROJECT_VERSION"
(
    cd "$pjsrc"
    if [ "$OS" = cygwin ]; then
        # pjproject takes Cygwin for Win32 - winsock, Win32 threads, WMME -
        # and Win32's headers then fight Cygwin's in everything that
        # includes pjsua.h. Cygwin is POSIX enough to build it as a Unix.
        sed -i -e 's/\*mingw\* | \*cygw\* | \*win32\* | \*w32\* )/*mingw* | *win32* | *w32* )/' \
               -e 's/\*cygwin\* | \*mingw\*)/*mingw*)/' aconfigure
        # Cygwin's pthread_key_t is a pointer, which pjlib keeps in a long
        # (lossless: both are 64 bits). GCC 14 made that and its kin errors.
        export CFLAGS="$CFLAGS -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration"
    fi
    # Audio and SIP only. Everything optional that would otherwise be
    # autodetected - and so differ from one build host to the next - is off,
    # and TLS is OpenSSL's, as in the Homebrew build datamodem was tested on.
    ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" \
        --disable-shared --disable-pjsua2 --disable-video --disable-libyuv \
        --disable-libwebrtc --disable-sdl --disable-ffmpeg --disable-v4l2 \
        --disable-openh264 --disable-vpx --disable-opencore-amr --disable-silk \
        --disable-opus --disable-bcg729 --disable-lyra --disable-upnp \
        --disable-libuuid --disable-darwin-ssl --with-ssl="$PREFIX" \
        >"$WORK/pjproject.log" 2>&1 || { tail -50 "$WORK/pjproject.log"; exit 1; }
    make dep >>"$WORK/pjproject.log" 2>&1 || { tail -50 "$WORK/pjproject.log"; exit 1; }
    make -j"$JOBS" lib >>"$WORK/pjproject.log" 2>&1 || { tail -50 "$WORK/pjproject.log"; exit 1; }
    make install >>"$WORK/pjproject.log" 2>&1 || { tail -50 "$WORK/pjproject.log"; exit 1; }
    grep -E '^(checking (for|if) .*(ssl|alsa|sound|audio)|Checking sound)' "$WORK/pjproject.log" || true
    mkdir -p "$PREFIX/share/licenses/pjproject"
    cp COPYING "$PREFIX/share/licenses/pjproject/"
)
touch "$PREFIX/.built-pjproject-$PJPROJECT_VERSION"
fi

# Belt and braces: the point of this prefix is that nothing in it is shared.
find "$PREFIX/lib" \( -name '*.so*' -o -name '*.dylib' -o -name '*.dll*' \) -print -delete

echo "==> done: $PREFIX"
