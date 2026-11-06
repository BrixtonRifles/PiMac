#!/bin/bash
# Builds "Floppy Browser.app" and the hfstool command into build/.
set -euo pipefail
cd "$(dirname "$0")"

# Universal (Apple silicon and Intel).
ARCHS=(--arch arm64 --arch x86_64)
swift build -c release "${ARCHS[@]}" --product FloppyBrowser
swift build -c release "${ARCHS[@]}" --product hfstool
BIN="$(swift build -c release "${ARCHS[@]}" --show-bin-path)"

if [ ! -f Resources/AppIcon.icns ]; then
	swift Resources/make-icon.swift Resources/AppIcon.icns
fi

APP="build/Floppy Browser.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/FloppyBrowser" "$APP/Contents/MacOS/"
cp Resources/Info.plist "$APP/Contents/"
cp Resources/AppIcon.icns "$APP/Contents/Resources/"
cp "$BIN/hfstool" build/
codesign --force --sign - "$APP"
echo "Built $APP and build/hfstool"
