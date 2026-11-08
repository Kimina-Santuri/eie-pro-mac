#!/bin/sh
# Builds a static, universal (arm64 + x86_64) libusb for macOS 12+ into build/libusb,
# so the driver doesn't depend on Homebrew's copy (built for the newest macOS only).
# Downloads the official libusb release tarball from GitHub.
set -e
cd "$(dirname "$0")/.."
VER=1.0.30
OUT="$PWD/build/libusb"
[ -f "$OUT/lib/libusb-1.0.a" ] && { echo "libusb already built in $OUT"; exit 0; }
WORK="$PWD/build/libusb-src"
rm -rf "$WORK" && mkdir -p "$WORK"
curl -fsSL "https://github.com/libusb/libusb/releases/download/v$VER/libusb-$VER.tar.bz2" -o "$WORK/libusb.tar.bz2"
tar -xjf "$WORK/libusb.tar.bz2" -C "$WORK"
for ARCH in arm64 x86_64; do
  B="$WORK/build-$ARCH"
  mkdir -p "$B"
  (cd "$B" && CFLAGS="-arch $ARCH -mmacosx-version-min=12.0 -O2" \
    "$WORK/libusb-$VER/configure" --host="$ARCH-apple-darwin" --disable-shared --enable-static \
      --prefix="$B/prefix" >/dev/null && make -j8 >/dev/null && make install >/dev/null)
done
mkdir -p "$OUT/lib" "$OUT/include"
lipo -create "$WORK/build-arm64/prefix/lib/libusb-1.0.a" "$WORK/build-x86_64/prefix/lib/libusb-1.0.a" \
  -output "$OUT/lib/libusb-1.0.a"
cp -R "$WORK/build-arm64/prefix/include/libusb-1.0" "$OUT/include/"
echo "Built $OUT/lib/libusb-1.0.a ($(lipo -archs "$OUT/lib/libusb-1.0.a"))"
