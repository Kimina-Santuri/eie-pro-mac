#!/bin/sh
# Install the EIE Pro Core Audio plug-in. Needs your password (sudo).
# Restarting the audio server cuts any playing sound for a second or two.
set -e
cd "$(dirname "$0")"
SRC="build/driver/EIEPro.driver"
DEST="/Library/Audio/Plug-Ins/HAL/EIEPro.driver"
[ -d "$SRC" ] || { echo "Build first: cmake -S driver -B build/driver && cmake --build build/driver"; exit 1; }
sudo rm -rf "$DEST"
sudo cp -R "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"
sudo killall coreaudiod
echo "Installed. 'EIE Pro' appears in Sound settings while the box is plugged in."
echo "Logs: log stream --predicate 'subsystem == \"co.ke.kimina.eie-pro\"'"
