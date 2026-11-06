#!/bin/bash
# Builds "Floppy Browser.app" and the hfstool command into build/.
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release --product FloppyBrowser
swift build -c release --product hfstool

if [ ! -f Resources/AppIcon.icns ]; then
	swift Resources/make-icon.swift Resources/AppIcon.icns
fi

APP="build/Floppy Browser.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/FloppyBrowser "$APP/Contents/MacOS/"
cp Resources/Info.plist "$APP/Contents/"
cp Resources/AppIcon.icns "$APP/Contents/Resources/"
cp .build/release/hfstool build/
codesign --force --sign - "$APP"
echo "Built $APP and build/hfstool"
