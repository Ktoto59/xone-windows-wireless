# Direct Windows GIP candidate

This development route carries radio input, vibration and PCM directly into
Windows' `XboxGipSynthetic` controller/audio interface. It does not create USB/IP
devices, run a USB/IP server or require usbip-win2. The physical USB adapter still
uses Windows' WinUSB driver and the same checked RAM firmware.

This is a user-mode transport using synthetic Windows devices, **not a new
certified kernel driver**. The API is undocumented. Developer Mode and an elevated
interactive user process were required on the development PC. Compatibility with
other Windows builds is not established. Xbox Accessories and firmware updates
are unavailable through this route. Update devices with a real USB cable.

## Development launch

Build with `scripts/Build-DirectGip.cmd`, which writes separate candidate EXEs
under `outputs/direct-gip/`; it does not replace a running USB bridge binary.
Then run `scripts/Run-DirectGip.ps1` and approve Windows UAC. The launcher stops
this checkout's USB background session after approval and runs without a timer.
It does not change driver bindings, Developer Mode or Windows boot policies.

The first launch copies only previously configured peer addresses into private
`config/native.local.json`; it keeps the USB configuration intact. No USB model
profiles are needed. To pair a new device, use the adapter and device pairing
buttons. Newly paired addresses are saved locally. Known peers retain discovery
after the manual pairing window ends, and silent initial associations get at
most three targeted recovery attempts.

Stop with `scripts/Run-DirectGip.ps1 -Stop`. To return to the saved USB route,
stop direct GIP first, then run `scripts/Set-AutomaticBridge.ps1 -Mode Start`.
On repeated native process failure, the launcher attempts to restore a USB
background session only if that session was active when direct GIP was started.
It never removes the shared USBip installation.

## Audio changes and current limits

- Prefer the supported 24 kHz mono microphone / 48 kHz stereo render pair.
- Send one 8 ms render block on a valid physical headset microphone frame.
  Collapse delayed credits rather than sending a catch-up burst.
- Start and dispose audio hosts away from the radio thread; bound both PCM queues.
- Use MMCSS and high-resolution waits for native audio pipe workers; keep PCM
  analysis and periodic packet logging out of normal playback.
- Allow the exact observed 2024 headset PC opt-out interface after identification,
  including when an earlier authentication message failed. This carries plaintext
  audio and does not establish wireless authentication v3 or a CCMP key.

Synthetic audio currently requires a synthetic gamepad parent per headset, which
may occupy an XInput slot. This is not an audio-only Windows endpoint implementation.
Two-headset endpoint attribution and simultaneous-device operation still need
hardware validation on this route. The previous clean USB audio results do not
validate direct GIP audio. The updated C++ session checks use simulated transport;
they do not exercise the undocumented Windows API or prove sound quality.

## Application deployment

The development launcher requests UAC per explicit launch. It does not register
elevated autorun against this user-writable checkout. A distributable application
must install its privileged code under protected Program Files, register a fixed
interactive-user task during setup and keep user preferences separate. That
deployment and the simplified application integration are follow-up work; the
existing alpha.3 public installer still uses USB/GIP.
