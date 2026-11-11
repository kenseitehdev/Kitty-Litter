#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
: "${VERSION:?Set VERSION to the release version, for example 0.2.0}"
: "${MAINTAINER:?Set MAINTAINER to 'Name <email>'}"
command -v dpkg-deb >/dev/null 2>&1 || { echo 'dpkg-deb is required' >&2; exit 1; }
TARGET_ARCH=${ARCH:-${DEB_ARCH:-$(dpkg --print-architecture)}}
case "$TARGET_ARCH" in
  x64|x86_64|amd64) DEB_ARCH=amd64; BUILD_ARCH=x86_64 ;;
  x86|i386|i686) DEB_ARCH=i386; BUILD_ARCH=i686 ;;
  arm64|aarch64) DEB_ARCH=arm64; BUILD_ARCH=aarch64 ;;
  *) echo "Unsupported target architecture: $TARGET_ARCH" >&2; exit 1 ;;
esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kitty-litter-deb.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
PKG="$TMP/pkg"
mkdir -p "$PKG/DEBIAN"
make -C "$ROOT/platforms/linux" install PREFIX=/usr DESTDIR="$PKG" ARCH="$BUILD_ARCH" CC="${CC:-cc}"
cat > "$PKG/DEBIAN/control" <<EOF
Package: kitty-litter
Version: $VERSION
Section: utils
Priority: optional
Architecture: $DEB_ARCH
Maintainer: $MAINTAINER
Depends: libgtk-3-0, libsqlite3-0
Description: Review and safely quarantine stale user files
 Kitty Litter provides a review-first cleanup flow with restore support.
EOF
OUT="$ROOT/build/packages/kitty-litter_${VERSION}_${DEB_ARCH}.deb"
mkdir -p "$(dirname "$OUT")"
dpkg-deb --root-owner-group --build "$PKG" "$OUT"
printf 'Built %s\n' "$OUT"
