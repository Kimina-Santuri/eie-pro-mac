#!/bin/sh
# Builds "EIE Pro.app" (menu-bar control app). With --install, copies it to
# /Applications and opens it. Ad-hoc signed.
set -e
cd "$(dirname "$0")"
swift build -c release
APP="build/EIE Pro.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/EIEPro "$APP/Contents/MacOS/EIEPro"
cp Support/Info.plist "$APP/Contents/Info.plist"
codesign --force --sign - "$APP"
echo "Built $APP"
if [ "$1" = "--install" ]; then
  pkill -x EIEPro 2>/dev/null || true
  rm -rf "/Applications/EIE Pro.app"
  cp -R "$APP" "/Applications/EIE Pro.app"
  open "/Applications/EIE Pro.app"
  echo "Installed and opened /Applications/EIE Pro.app"
fi
