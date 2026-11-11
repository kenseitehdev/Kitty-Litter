#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
APP=${APP_PATH:-"$ROOT/build/Kitty Litter.app"}
CONTENTS="$APP/Contents"
MODULE_CACHE=${SWIFT_MODULE_CACHE_PATH:-"$ROOT/build/swift-module-cache"}
ARCH=${ARCH:-$(uname -m)}

case "$ARCH" in
    arm64|aarch64) TARGET_ARCH=arm64 ;;
    x64|x86_64) TARGET_ARCH=x86_64 ;;
    x86|i386|i686)
        echo "32-bit macOS is unsupported by this macOS 13+ app." >&2
        exit 2
        ;;
    *)
        echo "Unsupported macOS architecture: $ARCH (use arm64 or x64)." >&2
        exit 2
        ;;
esac

rm -rf "$APP"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources"
cp "$ROOT/Resources/Info.plist" "$CONTENTS/Info.plist"
cp "$ROOT/Resources/AppIcon.icns" "$CONTENTS/Resources/AppIcon.icns"
swiftc -O -parse-as-library -module-cache-path "$MODULE_CACHE" \
    -target "$TARGET_ARCH-apple-macosx13.0" \
    -framework AppKit -framework SwiftUI \
    -o "$CONTENTS/MacOS/KittyLitter" "$ROOT/Sources/KittyLitter/KittyLitter.swift"
find "$APP" -name '._*' -type f -delete
codesign --force --deep --sign - "$APP"
printf 'Built %s\n' "$APP"
