# AGENTS.md

Notes for coding agents (and people) working on this repo. Add a dated section at the
end when you change something: what changed, what was verified, what wasn't.

## What this is

A user-space driver for the Akai EIE Pro (USB `09e8:0010`, vendor-specific class
`ff/ff/ff`, USB 2.0 high speed) on modern macOS / Apple Silicon. Akai's 2012 driver was
an Intel kext and can't load here. Goal: make old EIE Pros usable again for free
(Santuri / Nairobi studios), and later maybe a small control app ("standalone
interface", like Focusrite Control) for the user.

## Files

| File | Role |
|---|---|
| `src/eie_usb.c/.h` | The USB engine: open, start-up sequence, iso/bulk streaming, output pacing, input decoding, stall watchdog. Float callbacks, 4 ch interleaved. |
| `tools/eie-test.c` | CLI: `info`, `tone`, `record FILE.wav`. Prints input meters and clock/drift stats. |
| `Makefile` | `make` → `build/eie-test`. Needs Homebrew libusb. |

## USB protocol (verified on firmware 1.04)

Facts from the eie-pro-linux README, checked on the user's unit:

- Interface 0 alt 1: EP `0x02` iso out (audio, 12 B per frame: 4 ch × 24-bit LE packed;
  40 microframes per transfer = 5 ms), EP `0x83` bulk in MIDI, EP `0x04` bulk out MIDI.
- Interface 1 alt 1: EP `0x81` iso in clock (one packet per ms; byte 0 = frames the box
  consumed since the previous packet), EP `0x86` bulk in audio (64 B per frame,
  bit-sliced: bits 0/1 of bytes 0–23 = in1/in3, of bytes 32–55 = in2/in4, MSB first).
- Start: set both alts to 1, then control requests 0x56 (firmware), 0x49, class rate
  get/set (`0x22/0xa2`, value `0x0100`, index `0x86` and `0x02`, 3-byte LE rate), then
  vendor `0x40 0x49 value 0x0032`. Register 0x49 read `0x12` on a cold box, `0x32` after.
- Output is paced from the clock endpoint (frames sent = frames the box consumed,
  clamped ±5% around nominal). Input only flows while output is streaming.

## Gotchas

- **Stalls from other USB software.** The clock and inputs stopped within ~2 s on every
  run while the Push 2 bridge (`push2-traktor`) had been running for hours: it created a
  new libusb context every 2 s and never freed them. With the bridge paused or freshly
  restarted, streaming was rock solid. Fixed in the bridge on 2026-11-08 (one cached
  backend). The engine also has a watchdog: no clock packets for 100 ms → drain, re-run
  the start-up sequence, carry on (`eie_stats_t.restarts`).
- The first few ms of input after a start can hold a full-scale click: 10 ms are dropped.
- zsh doesn't word-split `$var`: `set -- $cfg` in a loop silently breaks.

## Plan

1. ✅ USB engine + CLI (tone, record).
2. Core Audio plug-in (AudioServerPlugIn in `/Library/Audio/Plug-Ins/HAL`) running the
   engine, so the EIE Pro is a normal 4-in/4-out device. Installing it needs `sudo`
   (the user runs it). Check the coreaudiod sandbox: the plug-in's Info.plist must
   allow the USB user-client classes.
3. MIDI in/out as a CoreMIDI device.
4. Sample-rate switching from Audio MIDI Setup, sleep/unplug handling.
5. Later: a small control app; Windows (much harder: signed drivers).

## 2026-11-08: project started

- Read the device descriptors, found the eie-pro-linux protocol notes, wrote the engine
  and `eie-test` from scratch (MIT).
- Verified: firmware 1.04 read; rate switch 44.1 → 48 kHz; 4 inputs stream continuously
  at 48 kHz (noise floor about −89 dBFS rms on empty inputs); output stays in step with
  the box's clock over 4–6 s runs; no transfer errors; the 10 ms input drop removes the
  start-up click.
- Played a 440 Hz tone at −30 dBFS on outputs 1–2: streamed without errors. **Whether it
  sounded clean is waiting on the user's ears.**
- Not verified: the watchdog's restart path (no way yet to force a stall on purpose; it
  did run once by mistake at start-up and streaming resumed); 44.1/88.2/96 kHz streaming;
  long runs; MIDI; unplug/replug.

## 2026-11-08 (later): crackle fixed in output pacing (pending the user's ears)

- The user heard the first tone "mostly steady" with some crackle. A trace (`make` with
  `-DEIE_TRACE` → `build/trace.txt`) showed why: the clock arrived in 8 ms batches while
  out transfers go every 5 ms, so chunk sizes swung 228/252 frames (±5%) every transfer.
  The box's buffer is small, so uneven chunks crackle.
- Now: clock read 1 ms per transfer (16 in flight); out chunks are nominal (240 at 48 kHz)
  steered by at most ±2 frames towards the lead learned in the first 200 ms. Trace:
  240 frames in 1562 of 1628 chunks, the rest 239/241; lead stays within a few frames.
- Played the tone again for 10 s: no errors, in step. Waiting on the user's ears.
- Second listen: steady part clean; crackle only at the start and the end. Added to the
  engine: output muted for 250 ms after every (re)start, then a 20 ms fade in; `eie_stop`
  fades out over 10 ms and keeps sending silence for ~60 ms before switching the box off.
  Third tone played (8 s, no errors). Waiting on the user's ears.
- Third listen: "less, really small tick at the beginning but way better". Accepted for
  now; the plug-in keeps the box streaming while apps use it.

## 2026-11-08 (later): Core Audio plug-in written and built

- Engine: render/capture callbacks now get a timeline position (box clock frames,
  kept across watchdog restarts); a DLL (≈0.5 Hz) over the 1 ms clock packets gives
  host time per frame, read lock-free with `eie_get_clock()`. Verified: locks to
  48 001 Hz after 3 s, 47 999 Hz after 10 s (±30 ppm).
- `driver/`: AudioServerPlugIn on libASPL v3.1.2 (MIT, vendored in
  `third_party/libASPL`, generated `.g.cpp` files already in the tree so its Python
  generators don't run). Device "EIE Pro", 4 in / 4 out float, 48 kHz only for now,
  zero timestamps from the box clock (period 16384), safety offset 1024, output/input
  rings of 32768 frames, IOKit hotplug notifications (no polling), libusb linked
  statically, ad-hoc signed. Info.plist allows the USB user-client classes for the
  coreaudiod sandbox.
- Built after the user's go-ahead (they also added a user-level auto-mode allow rule in
  `~/.claude/settings.json`: building open-source-licensed code locally is fine; no sudo,
  no system installs). `build/driver/EIEPro.driver`: ad-hoc signed, entry point
  `EieProEntryPoint`, links only system frameworks (libusb static), 760 KB.
- Build warnings: libASPL header designator order (harmless); Homebrew's libusb.a is
  built for macOS 26, deployment target here is 12.0. For older Macs, build libusb
  ourselves with a lower target before sharing.
- `install.sh` / `uninstall.sh` (sudo; restart coreaudiod). Not installed yet: the user
  runs `install.sh`. If the audio server misbehaves, `uninstall.sh` puts it back.

## 2026-11-08 (later): plug-in installed; dropouts inside the audio server fixed

- Installed with an admin password dialog (`osascript … with administrator privileges`);
  `sudo` can't prompt from Claude Code's `!`. The admin copy can't read `~/Documents`
  (privacy protection), so the bundle is staged in the scratchpad first. `install.sh`
  still works from a real Terminal.
- Loaded first time: the coreaudiod sandbox allowed the USB user clients; device "EIE Pro"
  4 in / 4 out at 48 kHz. macOS made it the **default output and system-sound device**.
- `tools/ca-tone.c` (`build/ca-tone`): plays through the Core Audio device by UID without
  changing the default device, meters the inputs.
- Dropouts: the user heard two short dropouts with crackle. Logs (now in the engine via
  `eie_set_log`) showed the box halting (clock and input stop, output still sent). Cause:
  inside the audio server, transfer completions were sometimes 14–22 ms late, longer than
  the output queue, so the box ran dry. libusb on macOS delivers every completion through
  its own thread `org.libusb.device-hotplug` at normal priority.
- Fixes: output queue 12 × 2 ms (24 ms, was 3 × 5 ms), safety offset 1536; our USB thread
  real-time with a 2 ms period; libusb's completion thread found by name and given
  real-time too; libusb started from a USER_INTERACTIVE queue. Restarts per 30 s:
  10 → 2 → 1 → 0. Verified: 60 s inaudible run with 0 restarts, longest clock gap 7.8 ms;
  15 s audible tone, 0 restarts, gap 5.6 ms. Waiting on the user's ears for the last one.
- Not verified: real music for a long time, other apps, sleep/wake, unplug/replug.
- User confirmed the 15 s tone: "perfect, no drops or crackles, just the tick at the
  beginning".

## 2026-11-08 (later): sample rates 44.1 / 48 / 88.2 / 96 kHz

- Device advertises the four rates; streams advertise matching float formats.
  `EieDevice::ApplyRate` (inside a HAL configuration change) sets the device rate, both
  streams, and restarts the engine if streaming. `EieStream` forwards a stream-format
  rate change to the device. Safety offset = 32 ms at any rate (`GetSafetyOffset`).
- `ca-tone` takes a 4th argument: sets the device rate first (like Audio MIDI Setup).
- Verified (10 s inaudible each, then 6 s audible at 44.1 and 96): every rate streams
  with 0 restarts, 0 errors, longest clock gap ≤ 6.3 ms. macOS stops IO, changes rate,
  restarts IO. Left at 48 kHz.
- After a reinstall the old driver copy can hold the box ~4 s (LIBUSB_ERROR_ACCESS);
  open now retries 15 times, 1 s apart (rebuilt, not yet reinstalled).

## 2026-11-08 (later): sleep / lid close

- Driver registers for system power (`IORegisterForSystemPower`, works in the coreaudiod
  sandbox). Will-sleep: stop the box if streaming. Has-powered-on: after 1.5 s, resume if
  Core Audio still wants I/O and nothing restarted it yet (`streaming` flag); if idle,
  reopen the USB handle. Any failed engine start reopens the box once and retries
  (`StartWithRecoveryLocked`). Unplug clears `ioRunning`/`streaming`.
- Verified with the user closing the lid during a 150 s inaudible stream: the box stopped
  before sleep; a 14 s dark wake streamed fine; full wake resumed; whole stream 0 restarts,
  0 errors. Power log: macOS sends will-sleep at display-off, sleep came 30 s later
  (Ableton "Live" delayed it); a maintenance sleep after a dark wake sends no will-sleep,
  but Core Audio stops I/O first. First version restarted twice on wake (Core Audio's
  StartIO plus ours); fixed with the `streaming` flag, reinstalled, 8 s tone clean.
- Not verified yet: unplug/replug, wake with nothing playing then play.

## 2026-11-08 (later): unplug/replug verified; MIDI engine written, CoreMIDI blocked in the driver

- Unplug/replug (user did it): "disconnected" then "connected" in the log, macOS made it
  the default output again, 6 s tone clean, 0 restarts.
- MIDI in the engine (untested, no MIDI cables yet): bulk 0x83 in (raw bytes, 0xfd
  padding stripped), bulk 0x04 out (9 B: up to 3 bytes, 0xfd pad, 0xe0 last) fed from a
  lock-free ring by `eie_midi_send`; pumped on the USB thread. Only while audio streams.
- CoreMIDI from inside the driver does NOT work: `MIDIClientCreate` → -10833 (can't reach
  the MIDI server), no sandbox denial logged. The driver runs in the system domain
  (`_coreaudiod`), the MIDI server is per user session. Listing the midiserver Mach
  services in Info.plist doesn't help (left in, harmless; the failed `Midi::Start` just
  logs).
- Plan: a small user-session app ("EIE Pro", menu bar) hosts the CoreMIDI ports and talks
  to the driver through libASPL custom properties (MIDI bytes as CFData; driver signals
  new input with a property-changed notification). The same app becomes the control app
  (status, sample rate, later mixer/monitoring).

## 2026-11-08 (later): control app, keep-awake, MIDI via the app; macOS AirPlay loop

- Driver: the in-driver CoreMIDI attempt is gone. Custom properties on the device for the
  app: `eSta` (status dict: connected, firmware, rate, streaming, keepAwake, restarts,
  errors, 4 input peaks since last read, MIDI byte totals), `eKpA` (keep awake 0/1: the
  box keeps streaming with no app using it, so no start tick and MIDI without audio),
  `eMIn` (MIDI bytes from the box, drained on read; the driver posts a property change when
  bytes arrive), `eMOu` (set: raw MIDI bytes to send). StartIO joins a kept-awake stream;
  StopIO leaves it running; wake and hotplug respect keep-awake.
- `app/`: "EIE Pro.app", SwiftUI menu-bar app (Swift package + `build.sh [--install]`,
  ad-hoc signed, LSUIElement). Status, sample-rate picker, 4 input meters, keep-awake,
  MIDI ports "EIE Pro MIDI In" / "EIE Pro MIDI Out" (CoreMIDI works from a user-session
  app), open at login. `@State` can't be used with the command line tools (no SwiftUI
  macro plug-in), so view state lives in the ObservableObject.
- Verified: the app builds and runs; both MIDI ports appear in CoreMIDI. NOT verified:
  the custom properties end to end (the audio server got stuck before the test, below).
- **macOS bug hit:** after many `killall coreaudiod` restarts in one boot, coreaudiod loops
  "Registering remote driver with bundle id com.apple.AirPlayXPCHelper" at 140% CPU and
  stops answering, so all Mac audio dies. The count doubles with every coreaudiod restart
  (2, 4, 8 … 3,000+/min). Not caused by this driver: it continued with the driver
  removed. AirPlayXPCHelper is SIP-protected (`launchctl kickstart -k` doesn't restart
  it), so only a reboot clears it. Lesson: restart coreaudiod as few times as possible
  per boot while developing; batch driver changes; reboot if audio misbehaves after
  many reinstalls.
- State at the end: driver REMOVED from /Library/Audio/Plug-Ins/HAL (to rule it out), app
  quit. After the reboot: reinstall the driver once, run the property test
  (`scratchpad/proptest.swift` equivalent), open the app.
- Follow-up: the user's sound then jumped to their iPhone. The Mac had NOT been rebooted
  yet (uptime 4 h 49 min); coreaudiod was still looping, and apps polling audio (WhatsApp,
  Kilele, Control Center, NIHardwareAgent, avconferenced/callservicesd) spun at 40–60% CPU
  each, load average 44. Continuity pulled the iPhone in as an audio route. Stuck test
  scripts of ours were killed. Fix: full restart (user restarting now).
- After the restart: check default output/input (switch back from the iPhone if needed,
  e.g. System Settings → Sound), check coreaudiod CPU and that the AirPlay message count is
  low, then install the driver ONCE, run the property test, open the app, push 4fe7d05+.

## 2026-11-08 (evening): after the restart: driver reinstalled, properties verified, app CPU fixed

- Restart cleared the coreaudiod AirPlay loop: 0 "AirPlayXPCHelper" messages, coreaudiod
  ~0.6% CPU, default output back on the Scarlett (not the iPhone).
- Install note: `sudo` can't prompt from Claude Code's `!` shell (no terminal), and a root
  shell from `osascript ... with administrator privileges` can't read ~/Documents (privacy
  protection: "Operation not permitted"). What worked: copy `build/driver/EIEPro.driver`
  to a temp folder, then osascript-with-admin `rm` / `cp` / `chown` / `killall coreaudiod`
  from there (native password dialog). One coreaudiod restart this boot.
- Property test (Swift, outside the app) against the installed driver, all pass: `eSta`
  read (firmware 1.04, 48 kHz, 0 restarts/errors); `eKpA` 1 starts streaming with no
  client, 0 stops it; peaks ~-77 dBFS (input noise floor) while streaming; `eMOu` 6 bytes
  accepted, `midiOut` = 6 (nothing on the wire checked: no MIDI cable); `eMIn` empty.
- App: was 36% CPU constantly. Cause: every 1/15 s poll re-assigned every @Published
  value, so SwiftUI redrew the menu-bar label and menu even when closed. Fix: assign only
  on change; poll at 15 Hz only while the menu is open (`menuOpen`, set by
  onAppear/onDisappear), 1 Hz otherwise; meters reset to -90 when not streaming (the
  driver's last peaks stay stale after a stop). Now ~0% with the menu closed.
- Not verified: CPU and meters with the menu open (needs a click), MIDI on the wire,
  MIDI in.

## 2026-11-08 (evening): output meters

- Driver: `Render` (the mix actually sent to the box) records per-channel peaks into
  `outPeak[]`; `eSta` gains `outPeaks` next to `peaks` (both drained on read).
- App: "Outputs" meters 1–4 under the inputs; shared `meter()` helper; meters show -90
  when not streaming. Reinstalled driver + app (second coreaudiod restart this boot, no
  AirPlay loop: 0 messages).
- Measured: driver process ~28% CPU while streaming (Brave playing through it), almost
  all of it kernel time on the libusb event thread (prio 97: 10 s system vs 1.3 s user):
  the 1 ms USB transfers (CLOCK_PKTS 1), not the metering. Idle (not streaming) it is
  <1%. Possible later work: fewer, larger transfers, but that touches the pacing that
  gives clean audio; only with listening tests.
- Not verified: the user seeing the output meters move.

## 2026-11-08 (evening): driver CPU, step 1 (bigger input transfers)

- Input bulk transfers 2048 → 16384 B (32 → 256 frames; 1500 → ~190 per second at
  48 kHz). Extra input delay ≤ 5.3 ms, inside the 32 ms safety offset.
- Measured with Brave playing through the box: driver process 28% → ~21% CPU, 0 restarts,
  0 errors, AirPlay loop still absent (third coreaudiod restart this boot).
- Remaining: clock 1000 transfers/s (CLOCK_PKTS 1) and out 500/s (OUT_PKTS 16). Next
  candidates: CLOCK_PKTS 2 (pacing still sees every out chunk), then 4 ms out transfers
  (OUT_PKTS 32 × 6, same 24 ms queue). Both touch output timing: listening test each.
- Front-panel needle meters don't move (reported by the user): manual says IN/OUT and
  1/2 / 3/4 switches pick the source, nothing about the computer. Waiting on the user's
  answers about switch positions; suspect register 0x49 (0x12 cold, 0x32 after start-up)
  if they never move.
- Not verified: the user's ears on this build; recording timing with the bigger transfers.

## 2026-11-08 (evening): driver CPU, step 2 (4 ms output transfers)

- User: step-1 build sounds clean; the front-panel needle meters DO work (in and out),
  the song was just quiet. No meter work needed.
- Output transfers 12 × 2 ms → 6 × 4 ms (same 24 ms queued; 20 ms left after one
  completes, was 22). Pacing code already scales with OUT_MS; comments updated.
- Measured with Brave playing: ~21% → ~19% CPU; 0 restarts / 0 errors over ~1.5 min;
  no AirPlay loop (fourth coreaudiod restart this boot). Smaller gain than expected:
  the busy thread is still ~85% kernel time, so per-transfer count isn't the whole
  story. Next: profile the driver thread (needs root for `sample`), then CLOCK_PKTS 2.
- Not verified: the user's ears on this build.

## 2026-11-08 (evening): packaging for other Macs (zip + .pkg), app icon, uninstall button

- User: step-2 build sounds great; app looks good; works in Ableton. Output needles:
  can't be made more sensitive from software (analog meters on the box's own signal;
  no known register); advice: send a hotter signal.
- App icon: own drawing in the style of the SF Symbol `waveform.circle.fill` (SF Symbols
  licence forbids symbols in app icons): `package/make-icon.swift` → `app/Support/AppIcon.icns`.
- App: "Uninstall…" button (confirm, unregister login item, admin dialog removes driver +
  app, `pkgutil --forget co.ke.kimina.eie-pro`, restarts coreaudiod). Not clicked yet.
- `app/build.sh` now builds universal (arm64 + x86_64, one `--triple` build per arch:
  Swift 6.4 writes both to `.build/out/Products/Release`, so each is copied out, then lipo).
- `driver/CMakeLists.txt`: `LIBUSB_DIR` (default Homebrew) and `CMAKE_OSX_ARCHITECTURES`
  passed to libASPL; `DRIVER_VERSION` cache var. Vendored libASPL patched: it read its
  version from `git describe` (our repo's tags), now `-DLIBASPL_VERSION=3.1.2`.
- `package/`: `build-libusb.sh` (downloads libusb 1.0.30 release tarball, static
  universal, macOS 12+), `make-release.sh [version]` (driver + app universal → component
  pkg with pre/postinstall, non-relocatable → productbuild with welcome + MIT licence,
  macOS 14+ → zip with the pkg and "How to install.rtf" from `how-to-install.html`).
  Test mode: `LIBUSB_DIR=/opt/homebrew ARCHS=arm64 package/make-release.sh 0.1.0-test`.
- Verified (test mode): zip builds; pkg holds driver + app + icon + scripts; signatures
  valid after expand; version set on the built app only. `com.apple.provenance` xattrs
  (added to files made inside the Claude session, unremovable) end up as `._` entries and
  "write: Permission denied" from pkgbuild; harmless, absent when built from Terminal.
- NOT done: `build-libusb.sh` was blocked by Claude Code's auto mode (downloads + builds
  outside code); waiting on the user. Then: real universal build, local install test of
  the pkg, Gatekeeper "Open Anyway" flow on the M2, GitHub release v0.1.0.

## 2026-11-08 (night): release 0.1.0 built, installer tested here, GitHub release

- `build-libusb.sh` run by the user. Found: the macOS 27 SDK has `pipe2` (27.0+), libusb's
  configure picked it up, so the lib would call a missing function on older macOS. Fixed
  with `ac_cv_func_pipe2=no` (libusb falls back to pipe + fcntl). Homebrew's libusb has
  the same problem: never ship with it.
- `make-release.sh 0.1.0`: driver universal, minos 12.0; app universal, minos 14.0; only
  system libraries. Zip 900 KB.
- Installed the release pkg on this Mac through Installer.app (user clicked through):
  package receipt co.ke.kimina.eie-pro 0.1.0, driver root:wheel, app relaunched by
  postinstall 2 s after the coreaudiod restart. Straight after install: 4 watchdog
  restarts, because the old driver's helper process was still streaming for a few seconds
  (LIBUSB_ERROR_ACCESS ×4 on open) and the Mac was busy; then the user's Mac slept and
  the box was unplugged. Retest with the box back: 20 s inaudible stream, 0 restarts,
  0 errors, all inputs live; keep-awake test clean.
- **Gotcha: in zsh, `log` is a builtin.** `log show …` silently does nothing; use
  `/usr/bin/log`. Earlier "0 AirPlay messages" checks this session were invalid; the real
  count after 6 coreaudiod restarts was 33 per 5 min (low, but it doubles per restart).
- README: Download and install section, status and limits updated.
- Not verified: the Gatekeeper "Open Anyway" flow and the install on the M2 (next), Intel,
  the Uninstall button, MIDI with cables.

## 2026-11-08 (night): compared with ewan-m/akai-eie-pro-mac; next: latency

- Read github.com/ewan-m/akai-eie-pro-mac (GPL-2, started 2026-05-20, DriverKit dext +
  activator app, USBDriverKit iso/bulk, 1 ms sync like ours). Needs Reduced Security +
  `systemextensionsctl developer on` until Apple grants DriverKit entitlements. 48 kHz
  tested, no MIDI, no sleep/stall recovery/fades found. Ahead of us on latency: per-rate
  SetInputLatency/SetOutputLatency, selectable IO buffer size. GPL: borrow ideas, not code.
- README: "See also" section crediting it.
- **Next (user's choice, 2026-11-09): measure round-trip latency** in Ableton (output
  looped into an input, check recorded alignment / Driver Error Compensation), then report
  correct device/stream latency and safety offsets per rate if it's off.

## 2026-11-09: round-trip latency measured; start alignment fixed; latency reported

- New `tools/ca-latency.c` (`build/ca-latency [rate] [buffer] [out_ch] [in_ch] [level_dB]`;
  build: `clang -O2 -o build/ca-latency tools/ca-latency.c -framework CoreAudio -framework CoreFoundation`).
  Needs a cable from output N to input N (input gain low). One IOProc plays 8 windowed
  noise bursts, cross-correlates the input on the device's sample timeline, prints the
  hardware path, the part the driver doesn't report, and the round trip an app sees.
- Before: driver reported 0 latency; hardware path varied per engine start (48 kHz: 80,
  88 or 128 frames). Cause: `in_pos` was anchored to `c_total` when the first input
  bulk transfer arrived, which depends on clock-packet / callback order (~1 ms steps).
- Fix (`src/eie_usb.c`): input frame k after a (re)start is numbered `c_total + k`, set in
  `submit_all`; frames dropped by `skip_in` still advance `in_pos`. Now two clusters
  8 frames apart (one 512 B input packet): 64/72 at 44.1/48 kHz, ~74/~82 at 88.2/96.
- Driver (`EieStream::GetLatency`): 34 frames each way at 44.1/48, 39 at 88.2/96
  (round trip 68 / 78). Verified with ca-latency: error +2 frames at 48k, -3 at 96k.
- Still big: safety offsets 32 ms each way, so an app's round trip is ~76-90 ms. Next:
  shrink the engine's 24 ms output lead and the safety offsets (test for crackles).
- Ableton (user, 2026-11-09): recorded loopback clicks sit on the grid, ~0.001 s from the
  source; Driver Error Compensation not needed.
- Not verified: Intel; release build
  (local build links Homebrew libusb; release script builds its own).
- Install from Claude: root can't read ~/Documents (TCC), so stage the bundle in /tmp
  first, then `osascript ... with administrator privileges` (sudo via `!` can't prompt).

## 2026-11-09 (later): round trip cut from 76-90 ms to ~50 ms

- Safety offsets were 32 ms each way. Now per scope (`EieDevice::GetPropertyData` answers
  the input scope; libASPL has one value): output = queue + 4 ms, input = 256 frames
  (one bulk transfer) + input margin (default 10 ms). Published inside a HAL
  configuration change (`RequestConfigurationChange`): just overriding GetSafetyOffset
  and notifying left clients on the old cached value.
- Output queue is runtime (`eie_set_queue_ms`, 2-24 ms, default 24 = 6 x 4 ms; shorter
  shapes use 2 ms / 1 ms transfers). Custom properties `eQue` (queue ms) and `eMar` (input
  margin ms); status dict adds queueMs, marginMs, leadMax/leadMin (frames the engine reads
  ahead of the box clock), inLate/outLate (+Max) underrun counters. Not persisted: the
  driver defaults apply after every audio-server restart; the app doesn't set them yet.
- Tools: `tools/ca-glitch.c` (60 s 997 Hz loopback, sine-prediction residual catches
  clicks; `GLITCH_SELFTEST=1` fakes a 10-frame dropout at 5 s, caught), `tools/ca-timing.c`
  (show/set queue and margin, verifies via the status dict). Settings sent while a test's
  IO is stopping were sometimes dropped silently: leave a second between runs.
- Measured (48 kHz, loopback, idle Mac + all 10 cores loaded): engine lead = queue +0-2 ms.
  8 ms: box runs short. 12 ms: clean in 7 runs (edge). 16 ms: one box-level burst (driver
  counters 0) in ~13 runs. 20 ms: steady clean, but a burst ~1.3-1.5 s after 2 of 4 starts
  (driver counters 0; not explained). 24 ms: clean at start and steady. Input margin 2-4 ms
  glitched (USB callbacks up to 6-10 ms late); 8 had one failure after a 10.2 ms stall.
- Final (24 ms / 10 ms): 4 fresh starts and 60 s runs at 44.1, 48 (x2), 96 (x3) clean
  except one test-side overload at 96k/128 and one box stall + watchdog restart at
  96k/256 (USB thread stalled ~20 ms: clock gap 21.9 ms, out gap 19.4 ms; pre-existing,
  the reason for the 24 ms queue). Round trip with buffer 128: 51 ms at 44.1, 50 at 48,
  44 at 96; reported latency within 5 frames.
- Install from Claude: stage the bundle in the scratchpad, then `osascript ... with
  administrator privileges` (root can't read ~/Documents; `!` sudo can't prompt).
- Not verified: soft synth / monitoring feel in Ableton at buffer 128 (user next); Intel;
  release build. Ideas: find the ~20 ms USB-thread stalls (then 12-16 ms queue, ~40 ms
  round trip); 128-frame input transfers (-2.7 ms, more CPU); "Low latency" switch in the
  app via `eQue`.

## 2026-11-09 (afternoon): 0.1.1 prepared, NOT published; Mac needs a restart

- User verdict on the ~50 ms build: "Latency is good, no clicks or crackles", fine down to
  a 32-sample buffer in Ableton. Asked for 0.1.1 plus a lighter, modern look.
- App redesign (`app/Sources/EIEPro/App.swift`, uncommitted): frosted see-through panel
  (`GlassBackground`: NSVisualEffectView .popover behind-window, alpha 0.78, rounded 16 pt
  mask; the panel's own effect views lightened to alpha 0.35), slim vertical IN/OUT meters
  (mint→amber→coral gradient, peak hold 1.5 s then falls, clip dot, dB scale), switch rows,
  status pill. Peak hold values in `DeviceLink` (`holds`, `outHolds`). `EIEPro --preview`
  opens the menu contents in a normal window for screenshots (window-only capture shows
  the blur as flat grey; that's expected).
- Version bumped to 0.1.1 (app Info.plist, build 2; `make-release.sh` default). A 0.1.1
  release was built and installed with the package before the rounded-corner change; it
  must be rebuilt. `pkgbuild` prints 4x "write: Permission denied" under the session
  sandbox; the package was complete (checked with `pkgutil --expand-full`).
- **macOS bug hit:** every coreaudiod restart makes `AirPlayXPCHelper` register with
  coreaudiod twice as often as the last time (1, 2, 4 … ~3000 by 12:47 after ~11 restarts
  for driver installs). Then coreaudiod spins at 100-150% CPU, new Core Audio clients hang
  in HALSystem::CheckOutInstance, and every audio app crawls. `killall` and
  `launchctl kickstart -k system/com.apple.AirPlayXPCHelper` did not restart it (SIP).
  Only a Mac restart clears it. **Keep coreaudiod restarts few**: batch driver changes,
  install once; check with
  `log show --last 5m --predicate 'process == "coreaudiod"' | grep -c AirPlayXPCHelper`.
- Next session: after the restart, confirm coreaudiod is idle, `package/make-release.sh
  0.1.1`, install the pkg once, short `ca-glitch`/`ca-latency` check, screenshot the panel,
  commit, tag v0.1.1, `gh release create`, update the kimina.co.ke Downloads entry.

## 2026-11-09 (after restart): release 0.1.1 published

- Mac restarted; coreaudiod clean (2-3 AirPlayXPCHelper lines in its log, not thousands).
- `package/make-release.sh 0.1.1` rebuilt with the rounded panel; pkg checked with
  `pkgutil --expand-full` (app and driver both 0.1.1). The 4x "write: Permission denied"
  from pkgbuild is still only sandbox noise.
- Installed once: `installer -pkg` through an admin dialog. The root installer couldn't read
  the pkg inside ~/Documents ("package path specified was invalid"), so it was copied to
  /Users/Shared first. Driver loaded (EIE Pro, 4 in, default output), app running,
  coreaudiod still idle afterwards.
- Not re-measured: no loopback cable patched (`ca-latency` found no bursts). The driver has
  no changes since b80a7f1, which the user tested (~50 ms, clean down to 32 samples).
- Panel screenshot via `EIEPro --preview`: rounded corners and new meters look right.
- Committed, tagged v0.1.1, GitHub release with `EIE-Pro-0.1.1.zip`; kimina.co.ke Downloads
  entry updated to 0.1.1.
- Still open: install on the M2 (Gatekeeper flow), MIDI with cables, Intel, real see-through
  look checked in the menu bar by the user.
