#!/bin/sh
# Builds a release: universal driver + app, an installer package, and a zip with the
# installer and "How to install" note. Output: build/release/EIE-Pro-<version>.zip
# Usage: package/make-release.sh [version]   (default 0.1.1)
set -e
cd "$(dirname "$0")/.."
VER="${1:-0.1.1}"
ROOT="$PWD"
OUT="$ROOT/build/release"
rm -rf "$OUT" && mkdir -p "$OUT"

# 1. libusb, universal, macOS 12+. For a quick local test only:
#    LIBUSB_DIR=/opt/homebrew ARCHS=arm64 package/make-release.sh 0.1.0-test
ARCHS="${ARCHS:-arm64;x86_64}"
if [ -z "$LIBUSB_DIR" ]; then package/build-libusb.sh; LIBUSB_DIR="$ROOT/build/libusb"; fi

# 2. driver, universal
cmake -S driver -B build/release-driver -DCMAKE_BUILD_TYPE=Release -DDRIVER_VERSION="$VER" \
  -DLIBUSB_DIR="$LIBUSB_DIR" -DCMAKE_OSX_ARCHITECTURES="$ARCHS" >/dev/null
cmake --build build/release-driver >/dev/null
DRV="build/release-driver/EIEPro.driver"
echo "Driver: $(lipo -archs "$DRV/Contents/MacOS/EIEPro")"

# 3. app, universal (version from the argument)
app/build.sh >/dev/null
APP="app/build/EIE Pro.app"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VER" "$APP/Contents/Info.plist"
codesign --force --sign - "$APP" 2>/dev/null
echo "App: $(lipo -archs "$APP/Contents/MacOS/EIEPro")"

# 4. installer package
STAGE="$OUT/stage"
mkdir -p "$STAGE/root/Library/Audio/Plug-Ins/HAL" "$STAGE/root/Applications"
cp -R "$DRV" "$STAGE/root/Library/Audio/Plug-Ins/HAL/"
cp -R "$APP" "$STAGE/root/Applications/"
# No Finder metadata or download flags in the package (they become ._ files)
xattr -cr "$STAGE/root" 2>/dev/null || true
export COPYFILE_DISABLE=1
# Always install to /Applications, even if a copy of the app exists somewhere else
pkgbuild --analyze --root "$STAGE/root" "$STAGE/components.plist" >/dev/null
i=0; while plutil -extract "$i" xml1 -o /dev/null "$STAGE/components.plist" 2>/dev/null; do
  plutil -replace "$i.BundleIsRelocatable" -bool NO "$STAGE/components.plist"; i=$((i + 1)); done
pkgbuild --root "$STAGE/root" --component-plist "$STAGE/components.plist" \
  --scripts package/scripts --identifier co.ke.kimina.eie-pro --version "$VER" \
  --install-location / "$STAGE/EIEPro-component.pkg" >/dev/null
sed "s/@VERSION@/$VER/g" package/distribution.xml > "$STAGE/distribution.xml"
productbuild --distribution "$STAGE/distribution.xml" --resources package/resources \
  --package-path "$STAGE" "$STAGE/EIE Pro $VER.pkg" >/dev/null

# 5. zip: installer + note
DIR="$OUT/EIE Pro $VER"
mkdir -p "$DIR"
cp "$STAGE/EIE Pro $VER.pkg" "$DIR/"
sed "s/@VERSION@/$VER/g" package/how-to-install.html > "$STAGE/how-to-install.html"
textutil -convert rtf -output "$DIR/How to install.rtf" "$STAGE/how-to-install.html"
(cd "$OUT" && ditto -c -k --norsrc --noextattr --keepParent "EIE Pro $VER" "EIE-Pro-$VER.zip")
rm -rf "$STAGE"
echo "Release: $OUT/EIE-Pro-$VER.zip"
