# 0.1.0-alpha.6 candidate

This candidate brings the experimental runtime, recovery, diagnostics and Setup
work into the main release channel. Alpha.5.3-alpha.5.7 were local test candidates.
Merging the source does not publish release assets; alpha.5.2 remains the latest
published installer until a separate alpha.6 release is created.

## Changes since alpha.5.2

- Separate reliable control/lifecycle/GIP traffic from expiring playback and
  microphone media. Keep protocol ordering and report capacity errors explicitly.
- Move native physical GIP transmission to a bounded overlapped-USB executor,
  with real completion, cancellation and RF generation checks.
- Recover USB routes while retaining a live radio association, replay validated
  identity on re-import and apply bounded recovery backoff.
- Add wake-capable idle power with a fallback receive window; connected clients
  and pairing prevent idle suspension.
- Keep native status/counters in shared memory and use Windows events for stop
  commands. Read configuration/firmware at startup and save learned pairing
  metadata at shutdown. Default installer builds omit runtime diagnostic journals.
- Retain a Python development/reference path with detailed traces and a separate
  anomaly journal; diagnostics are opt-in and excluded from the normal payload.
- Add Radio Wave branding to Windows executables and Setup. Integrate independent
  start-now/sign-in options, remembered upgrade choices and Start menu controls.
- Add architecture/traffic diagrams, including a compact diagram in the README.

## Validation and remaining limits

The experimental head passed Windows C++ builds, protocol/session, USB/IP
core/codec, loopback fault/recovery, Python parity, memory IPC, cancellation and
maintenance tests. Setup smoke checks cover payload hashes/privacy, icon/shortcut
registration, all four startup-option combinations, remembered upgrade choices
and removal under a separate AppId with hardware hooks disabled.

Earlier local hardware use reported one to two hours of generally stable playback
with occasional micro interruptions. Later IO-isolation changes have software
coverage but do not establish a measured audible improvement. Cold/warm adapter
reconnect, physical USB error 31, long-running voice/music, fresh-PC driver recovery
and broad sleep/resume still need hardware validation. The Windows virtual bus
remains usbip-win2; this is an alpha, not stock-driver reliability parity.

Install the newer Setup as the same Windows user; an uninstall is not required
first. Retain private recovery data. See [installation](../INSTALL.md),
[limitations](../LIMITATIONS.md) and [runtime IO](../RUNTIME-IO.md).

**Do not update headset/controller firmware through the bridge, including OTA.**
Use a physical USB data cable and the manufacturer's supported update procedure.
