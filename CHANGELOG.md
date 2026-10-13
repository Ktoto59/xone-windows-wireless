# Changelog

## 0.1.0-alpha.2 — 2026-10-13

- Windows x64 Setup EXE with per-user installation, Start menu/desktop shortcuts
  and an uninstall entry.
- First-run WinForms control panel for dependency preparation, adapter binding,
  association discovery and device mapping, bridge start/stop and stock restoration.
- Isolated official Python 3.13.16 runtime included in Setup with its license.
  USBip and firmware are obtained separately from their official sources and checked.
- Uninstall/upgrade checks for a stopped bridge; removal restores only the saved
  binding owned by that installation and refuses deletion when restoration fails.
- Unlimited launcher sessions with `-Seconds 0`, used by the control panel.
- Setup helper tests for checksum rejection, atomic mapping, radio-address parsing,
  process identities and uninstall guards. No radio/audio protocol changes.

Headset 2021 reconnection and Accessories' Connected battery label remain unresolved.
The installer does not turn this user-mode/virtual USB prototype into a kernel driver.

## 0.1.0-alpha.1 — 2026-10-13

First public snapshot of the working Windows x64 research prototype.

- xone-derived WinUSB/MT76 radio transport, client handling, supported v1/v2
  authentication, protocol parsing and fragment/ACK handling.
- Captured native USB/GIP profiles for Series, Headset 2021 and Headset 2024.
- Windows Xbox Accessories detection/settings opening for tested hardware.
- Native XInput input and short clean stereo headset-tone validation.
- Radio-clock render pacing and eight-slot USB/ISO audio adaptation.
- Per-device brokers and ownership checks for concurrent devices.
- Radio-to-USB fragmentation preserving original ACK boundaries.
- Physical battery observation and read-only WGI battery diagnostics.
- Snapshot locking/recovery fixes, with a live blocking-file regression check.
- Public device configuration without embedded owner MACs or adapter serials.

Known issues include Headset 2021 requiring re-pairing/search recovery after a
power cycle, Accessories' Connected battery label, unimplemented wireless auth v3,
unverified raw-peer RF encryption, no firmware-update forwarding, and incomplete
long-term/audio/reconnection validation. See README for the full scope.

Local hardware observations preceded publication cleanup. The public wrapper uses
private per-user configuration instead of the development machine's fixed IDs.
Protocol/radio/audio core behavior is retained. Software checks validate the
configuration changes; broad hardware compatibility is not claimed.
