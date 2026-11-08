#!/bin/sh
# Builds "EIE Pro.app" (menu-bar control app), universal (Apple Silicon + Intel), macOS 14+.
# With --install, copies it to /Applications and opens it. Ad-hoc signed.
set -e
cd "$(dirname "$0")"
APP="build/EIE Pro.app"
BIN="build/bin"
rm -rf "$BIN" && mkdir -p "$BIN"
# One architecture per build (each build writes to the same products folder), then join.
for ARCH in arm64 x86_64; do
  swift build -c release --triple "$ARCH-apple-macosx14.0"
  OUT=$(find .build -path '*elease*' -name EIEPro -type f -perm +111 | xargs ls -t | head -1)
  [ "$(lipo -archs "$OUT")" = "$ARCH" ] || { echo "Unexpected binary for $ARCH: $OUT"; exit 1; }
  cp "$OUT" "$BIN/EIEPro-$ARCH"
done
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create "$BIN/EIEPro-arm64" "$BIN/EIEPro-x86_64" -output "$APP/Contents/MacOS/EIEPro"
cp Support/Info.plist "$APP/Contents/Info.plist"
cp Support/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
codesign --force --sign - "$APP"
echo "Built $APP ($(lipo -archs "$APP/Contents/MacOS/EIEPro"))"
if [ "$1" = "--install" ]; then
  pkill -x EIEPro 2>/dev/null || true
  rm -rf "/Applications/EIE Pro.app"
  cp -R "$APP" "/Applications/EIE Pro.app"
  open "/Applications/EIE Pro.app"
  echo "Installed and opened /Applications/EIE Pro.app"
fi
