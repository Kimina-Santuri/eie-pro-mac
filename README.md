# EIE Pro for Mac

A new, open-source driver for the **Akai Professional EIE Pro** USB audio interface on
modern Macs, including Apple Silicon. Akai's driver dates from 2012 and hasn't worked on
current macOS for years, so the box usually ends up unused on a shelf.

> **Status: early and experimental, but working.** On an Apple Silicon Mac (macOS 27),
> the EIE Pro shows up as a normal 4-in / 4-out sound device at 44.1, 48, 88.2 or
> 96 kHz, plays and records cleanly, and survives sleep, lid close and unplugging.
> Not yet tried in DJ software or a DAW. MIDI and a small control app are being built.
> Use at your own risk.

## How it works

The EIE Pro isn't a standard ("class compliant") USB audio device, so macOS won't use it
without a driver. This one talks to it from user space with [libusb](https://libusb.info)
inside a Core Audio plug-in. No kernel extension, no DriverKit entitlement, no lowered
Mac security.

```
App ⇄ Core Audio ⇄ EIEPro.driver (libASPL plug-in) ⇄ eie_usb.c (libusb, real-time) ⇄ USB ⇄ EIE Pro
```

The Mac follows the EIE Pro's own clock, so long sessions don't drift.

## Install (build from source)

You need the Xcode command line tools, CMake and Homebrew's libusb:

```sh
brew install cmake libusb
cmake -S driver -B build/driver -DCMAKE_BUILD_TYPE=Release
cmake --build build/driver
./install.sh        # asks for your password, restarts the Mac's audio for a second
```

Then pick **EIE Pro** in System Settings → Sound, or in your DJ software / DAW.
`./uninstall.sh` removes it.

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
- Homebrew's libusb targets recent macOS; for older Macs build libusb with a lower
  deployment target first.
- A small tick when the box starts streaming.
- MIDI: the USB side is written but untested; the ports will come from the control app.
- No Windows version.

## Thanks

The USB protocol was worked out by the
[eie-pro-linux](https://github.com/MatTaylorSharrow/eie-pro-linux) project. Its
README's protocol notes made this possible. This code is a separate, from-scratch
implementation.

## Licence

MIT. See [LICENSE](LICENSE). Not affiliated with or endorsed by Akai Professional or
inMusic; "Akai Professional" and "EIE" are their trademarks.
