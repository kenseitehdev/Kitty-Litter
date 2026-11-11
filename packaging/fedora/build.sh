#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
: "${VERSION:?Set VERSION to the release version, for example 0.2.0}"
command -v rpmbuild >/dev/null 2>&1 || { echo 'rpmbuild is required' >&2; exit 1; }

ARCH=${ARCH:-$(uname -m)}
case "$ARCH" in
  arm64) RPM_ARCH=aarch64 ;;
  x64) RPM_ARCH=x86_64 ;;
  x86) RPM_ARCH=i686 ;;
  *) RPM_ARCH=$ARCH ;;
esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kitty-litter-rpm.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
STAGE="$TMP/stage"
TOP="$TMP/rpmbuild"
mkdir -p "$STAGE" "$TOP/BUILD" "$TOP/BUILDROOT" "$TOP/RPMS" "$TOP/SOURCES" "$TOP/SPECS" "$TOP/SRPMS"
make -C "$ROOT/platforms/linux" install PREFIX=/usr DESTDIR="$STAGE" ARCH="$ARCH" CC="${CC:-cc}"
install -D -m 0644 "$ROOT/LICENSE" "$STAGE/usr/share/licenses/kitty-litter/LICENSE"
tar -C "$STAGE" -czf "$TOP/SOURCES/kitty-litter-files.tar.gz" .
cat > "$TOP/SPECS/kitty-litter.spec" <<EOF
Name: kitty-litter
Version: $VERSION
Release: 1%{?dist}
Summary: Review and safely quarantine stale user files
License: MIT
Source0: kitty-litter-files.tar.gz
BuildArch: $RPM_ARCH

%description
Kitty Litter provides a review-first cleanup flow with restore support.

%prep

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -xzf %{SOURCE0} -C %{buildroot}

%files
%license /usr/share/licenses/kitty-litter/LICENSE
%attr(0755,root,root) /usr/bin/kitty-litter
%attr(0644,root,root) /usr/share/applications/kitty-litter.desktop
%attr(0644,root,root) /usr/share/icons/hicolor/256x256/apps/kitty-litter.png
EOF
rpmbuild --target "$RPM_ARCH" --define "_topdir $TOP" -bb "$TOP/SPECS/kitty-litter.spec"
OUT="$ROOT/build/packages/fedora"
mkdir -p "$OUT"
cp "$TOP/RPMS/$RPM_ARCH/kitty-litter-$VERSION-1"*.rpm "$OUT/"
printf 'Built Fedora RPM for %s in %s\n' "$RPM_ARCH" "$OUT"
