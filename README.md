# EIE Pro for Mac

A new, open-source driver for the **Akai Professional EIE Pro** USB audio interface on
modern Macs, including Apple Silicon. Akai's driver dates from 2012 and hasn't worked on
current macOS for years, so the box usually ends up unused on a shelf.

> **Status: early, but working.** On an Apple Silicon Mac, the EIE Pro shows up as a
> normal 4-in / 4-out sound device at 44.1, 48, 88.2 or 96 kHz, plays and records
> cleanly in Ableton Live, and survives sleep, lid close and unplugging. A menu-bar app
> shows input and output meters, sets the sample rate and hosts the MIDI ports.
> Use at your own risk.

## Download and install

1. Download **EIE-Pro-x.y.z.zip** from the
   [latest release](https://github.com/Kimina-Santuri/eie-pro-mac/releases/latest) and
   double-click it.
2. Follow **How to install** in the folder. In short: open the `.pkg`; when macOS says it
   can't verify it (it isn't signed with a paid Apple account), click Done, then
   System Settings → Privacy & Security → **Open Anyway**, and install.
3. Plug in the EIE Pro and pick **EIE Pro** in System Settings → Sound or in your music app.

Needs macOS 14 or newer, Apple Silicon or Intel. To remove it: menu-bar icon → **Uninstall…**

## How it works

The EIE Pro isn't a standard ("class compliant") USB audio device, so macOS won't use it
without a driver. This one talks to it from user space with [libusb](https://libusb.info)
inside a Core Audio plug-in. No kernel extension, no DriverKit entitlement, no lowered
Mac security.

```
App ⇄ Core Audio ⇄ EIEPro.driver (libASPL plug-in) ⇄ eie_usb.c (libusb, real-time) ⇄ USB ⇄ EIE Pro
```

The Mac follows the EIE Pro's own clock, so long sessions don't drift.

## Build from source

You need the Xcode command line tools, CMake and Homebrew's libusb:

```sh
brew install cmake libusb
cmake -S driver -B build/driver -DCMAKE_BUILD_TYPE=Release
cmake --build build/driver
./install.sh        # asks for your password, restarts the Mac's audio for a second
```

Then pick **EIE Pro** in System Settings → Sound, or in your DJ software / DAW.
`./uninstall.sh` removes it. The menu-bar app: `app/build.sh --install`.

A release (universal driver + app, installer, zip with the install note):
`package/make-release.sh 0.1.0` → `build/release/EIE-Pro-0.1.0.zip`. It builds its own
universal libusb for macOS 12+ (`package/build-libusb.sh`), so Homebrew's isn't used.

## Test tools

```sh
make                                   # build/eie-test: talks to the box directly
./build/eie-test tone -l -30           # (only when the driver isn't installed)
cc -o build/ca-tone tools/ca-tone.c -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
./build/ca-tone 5 -30 440 48000        # quiet tone through the installed driver
```

Logs: `log stream --predicate 'subsystem == "co.ke.kimina.eie-pro"'`

## Known limits

- Built and tested on one EIE Pro (firmware 1.04) and one Apple Silicon Mac.
- The installer is not signed or notarised (no paid Apple developer account), so
  macOS asks you to allow it once in Privacy & Security.
- Intel Macs: built for, not tested.
- A small tick when the box starts streaming.
- MIDI: ports "EIE Pro MIDI In / Out" come from the menu-bar app; not yet tested with
  cables.
- The driver uses about 20% of one CPU core while audio plays (USB traffic every
  millisecond); under 1% when idle.
- No Windows version.

## Thanks

The USB protocol was worked out by the
[eie-pro-linux](https://github.com/MatTaylorSharrow/eie-pro-linux) project. Its
README's protocol notes made this possible. This code is a separate, from-scratch
implementation.

## Licence

MIT. See [LICENSE](LICENSE). Not affiliated with or endorsed by Akai Professional or
inMusic; "Akai Professional" and "EIE" are their trademarks.
