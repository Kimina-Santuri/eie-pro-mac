#!/bin/sh
# Remove the EIE Pro Core Audio plug-in. Needs your password (sudo).
set -e
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/EIEPro.driver"
sudo killall coreaudiod
echo "Removed."
