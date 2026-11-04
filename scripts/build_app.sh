#!/bin/bash
# Builds build/KorMSX.app (release) with the free C-BIOS and the ROM dumps in
# the local roms/ folder inside the package (Contents/Resources/roms), so the
# app is self-contained. ROM dumps are copyrighted: a bundle with them is for
# personal use only. --no-roms builds a distributable bundle without them; it
# then finds ROMs in $KORMSX_ROMS, the folder chosen in the File menu, or
# roms/ next to the app.
set -euo pipefail
cd "$(dirname "$0")/.."

swift build -c release
APP=build/KorMSX.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/KorMSX "$APP/Contents/MacOS/KorMSX"
cp -R Sources/KorMSX/Resources/cbios "$APP/Contents/Resources/cbios"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>KorMSX</string>
    <key>CFBundleDisplayName</key><string>KorMSX</string>
    <key>CFBundleIdentifier</key><string>local.kormsx</string>
    <key>CFBundleExecutable</key><string>KorMSX</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>MSX Cartridge ROM</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>CFBundleTypeExtensions</key><array><string>rom</string><string>mx1</string><string>mx2</string></array>
        </dict>
    </array>
</dict>
</plist>
PLIST

if [[ "${1:-}" != "--no-roms" ]]; then
    mkdir -p "$APP/Contents/Resources/roms"
    find roms -maxdepth 1 -type f ! -name README.txt -exec cp {} "$APP/Contents/Resources/roms/" \;
    echo "ROMs bundled: $(ls "$APP/Contents/Resources/roms" | tr '\n' ' ')"
fi

codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "Built $APP"
